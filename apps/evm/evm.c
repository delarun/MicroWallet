// evm.c — общий код EVM-приложений (apps/bsc, apps/eth): адреса m/44'/60'/0'/0/i (как в
// MetaMask), перевод монеты сети и токенов из белого списка приложения.
//
// Приложение задаёт до #include: EVM_NAME, EVM_SHOW ("BSC" → «BSC #0»), EVM_CHAIN_MAIN/TEST,
// EVM_COIN_MAIN/TEST (символ монеты), EVM_NET_MAIN/TEST (название сети на экране),
// EVM_HIGH_FEE (wei, u64) и белые списки токенов EVM_TOKENS_MAIN/TEST —
// инициализаторы {{адрес 20 байт}, "символ", знаков}, ...
//
// Подписываются только переводы:
//   data пустая                     — монета сети на адрес to;
//   to = токен из белого списка,
//   value = 0, data = transfer(a,n) — токен.
// Вызовы контрактов (approve и любые другие), деплой — отказ.
//
// Транзакция приходит целиком, приложение само её разбирает и показывает:
//   legacy EIP-155: rlp([nonce, gasPrice, gas, to, value, data, chainId, 0, 0])
//   EIP-1559:       0x02 || rlp([chainId, nonce, maxPriorityFee, maxFee, gas, to, value, data, []])
// Подпись — keccak256 этих байт целиком; ответ r || s || recid (v / yParity считает хост).
//
// Команды: addr <i> | show <i> | sign <i> <hex>
#include "mw.h"

typedef struct {
  uint8_t addr[20];
  const char *symbol;
  int decimals;
} token_t;

static const token_t TOKENS_MAIN[] = EVM_TOKENS_MAIN;
static const token_t TOKENS_TEST[] = EVM_TOKENS_TEST;

typedef struct {
  int type;              // 0 — legacy, 2 — EIP-1559
  uint64_t nonce;
  uint8_t to[20];        // получатель (у токена — из data)
  const token_t *token;  // 0 — монета сети
  uint8_t amount[32];    // big-endian
  uint8_t max_fee[32];   // (gasPrice | maxFee) * gas, wei
} evm_tx_t;

static uint64_t chain_id(void) { return mw_net() ? EVM_CHAIN_MAIN : EVM_CHAIN_TEST; }
static const char *coin(void) { return mw_net() ? EVM_COIN_MAIN : EVM_COIN_TEST; }

// ---------- RLP (строгий: каноничная форма, длины по границам) ----------

typedef struct {
  const uint8_t *p, *end;
} rlp_t;

// Длина в префиксе: ll байт, без ведущего нуля, >= 56
static int rlp_long_len(rlp_t *r, size_t ll, size_t *n) {
  if (ll > 3 || (size_t)(r->end - r->p) < ll || r->p[0] == 0) return 0;
  *n = 0;
  for (size_t i = 0; i < ll; i++) *n = (*n << 8) | *r->p++;
  return *n >= 56;
}

// Строка RLP (не список)
static int rlp_str(rlp_t *r, const uint8_t **d, size_t *n) {
  if (r->p >= r->end) return 0;
  uint8_t b = *r->p++;
  if (b < 0x80) {
    *d = r->p - 1;
    *n = 1;
    return 1;
  }
  if (b <= 0xB7) *n = b - 0x80;
  else if (b <= 0xBF) {
    if (!rlp_long_len(r, b - 0xB7, n)) return 0;
  } else return 0;  // список
  if ((size_t)(r->end - r->p) < *n) return 0;
  if (*n == 1 && r->p[0] < 0x80) return 0;  // должно было быть одним байтом
  *d = r->p;
  r->p += *n;
  return 1;
}

// Заголовок списка: дальше ровно *n байт его содержимого
static int rlp_list(rlp_t *r, size_t *n) {
  if (r->p >= r->end) return 0;
  uint8_t b = *r->p++;
  if (b < 0xC0) return 0;
  if (b <= 0xF7) *n = b - 0xC0;
  else if (!rlp_long_len(r, b - 0xF7, n)) return 0;
  return (size_t)(r->end - r->p) >= *n;
}

// Беззнаковое целое: без ведущих нулей, не длиннее max байт; результат в be32
static int rlp_uint(rlp_t *r, size_t max, uint8_t be32[32]) {
  const uint8_t *d;
  size_t n;
  if (!rlp_str(r, &d, &n) || n > max || (n && d[0] == 0)) return 0;
  memset(be32, 0, 32);
  memcpy(be32 + 32 - n, d, n);
  return 1;
}

static uint64_t be_u64(const uint8_t be32[32]) {
  uint64_t v = 0;
  for (int i = 24; i < 32; i++) v = (v << 8) | be32[i];
  return v;
}

static int be_cmp(const uint8_t a[32], const uint8_t b[32]) {
  for (int i = 0; i < 32; i++)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

static const char *evm_parse(const uint8_t *buf, size_t len, evm_tx_t *tx) {
  static const uint8_t SEL[4] = {0xa9, 0x05, 0x9c, 0xbb};  // transfer(address,uint256)
  uint8_t num[32], fee_per_gas[32], tip[32], gas[32], value[32];
  const uint8_t *to, *data, *d;
  size_t to_n, data_n, n, body;
  memset(tx, 0, sizeof *tx);
  if (len && buf[0] == 0x02) {  // EIP-2718: тип 2
    tx->type = 2;
    buf++, len--;
  } else if (len && buf[0] < 0xC0) {
    return "unsupported transaction type";
  }
  rlp_t r = {buf, buf + len};
  if (!rlp_list(&r, &body) || (size_t)(r.end - r.p) != body) return "bad RLP";

  if (tx->type == 2) {
    if (!rlp_uint(&r, 8, num) || be_u64(num) != chain_id()) return "wrong chain id";
    if (!rlp_uint(&r, 8, num)) return "bad nonce";
    tx->nonce = be_u64(num);
    if (!rlp_uint(&r, 16, tip)) return "bad priority fee";
    if (!rlp_uint(&r, 16, fee_per_gas)) return "bad max fee";
    if (be_cmp(tip, fee_per_gas) > 0) return "priority fee above max fee";
  } else {
    if (!rlp_uint(&r, 8, num)) return "bad nonce";
    tx->nonce = be_u64(num);
    if (!rlp_uint(&r, 16, fee_per_gas)) return "bad gas price";
  }
  if (!rlp_uint(&r, 8, gas)) return "bad gas limit";
  if (!rlp_str(&r, &to, &to_n) || to_n != 20) return to_n == 0 ? "contract creation is not supported" : "bad recipient";
  if (!rlp_uint(&r, 32, value)) return "bad value";
  if (!rlp_str(&r, &data, &data_n)) return "bad data";
  if (tx->type == 2) {
    if (!rlp_list(&r, &n) || n != 0) return "access list not supported";
  } else {
    if (!rlp_uint(&r, 8, num) || be_u64(num) != chain_id()) return "wrong chain id";
    if (!rlp_str(&r, &d, &n) || n != 0) return "bad EIP-155 r";
    if (!rlp_str(&r, &d, &n) || n != 0) return "bad EIP-155 s";
  }
  if (r.p != r.end) return "trailing bytes";

  uint8_t fee[24];
  mw_mul(fee_per_gas + 16, 16, gas + 24, 8, fee);  // 128 × 64 -> 192 бита
  memcpy(tx->max_fee + 8, fee, 24);

  if (data_n == 0) {
    memcpy(tx->to, to, 20);
    memcpy(tx->amount, value, 32);
    return 0;
  }
  const token_t *list = mw_net() ? TOKENS_MAIN : TOKENS_TEST;
  size_t cnt = mw_net() ? sizeof TOKENS_MAIN / sizeof *TOKENS_MAIN : sizeof TOKENS_TEST / sizeof *TOKENS_TEST;
  for (size_t i = 0; i < cnt; i++)
    if (!memcmp(list[i].addr, to, 20)) tx->token = &list[i];
  if (!tx->token) return "unknown contract: only whitelisted tokens";
  // a9059cbb | 12 нулей + адрес | uint256 сумма. approve и прочее — нет
  if (data_n != 68 || memcmp(data, SEL, 4)) return "only transfer(address,uint256) is supported";
  if (!mw_is_zero(data + 4, 12)) return "bad token recipient";
  if (!mw_is_zero(value, 32)) return "coin sent with token transfer";
  memcpy(tx->to, data + 16, 20);
  memcpy(tx->amount, data + 36, 32);
  return 0;
}

// ---------- адреса ----------

static void path_of(uint32_t i, uint32_t p[5]) {
  p[0] = H(44);
  p[1] = H(60);
  p[2] = H(0);
  p[3] = 0;
  p[4] = i;
}

// EIP-55: регистр hex-символа задаёт соответствующий полубайт keccak(hex)
static void eip55(const uint8_t a[20], char out[43]) {
  char low[41];
  uint8_t h[32];
  mw_hex(a, 20, low);
  mw_keccak256(low, 40, h);
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < 40; i++) {
    int nib = i & 1 ? h[i / 2] & 15 : h[i / 2] >> 4;
    char c = low[i];
    out[2 + i] = (c >= 'a' && nib >= 8) ? (char)(c - 32) : c;
  }
  out[42] = 0;
}

static int address(uint32_t i, char out[43]) {
  uint32_t p[5];
  uint8_t pub[65], h[32];
  path_of(i, p);
  if (mw_pubkey(p, 5, pub)) return 0;
  mw_keccak256(pub + 1, 64, h);
  eip55(h + 12, out);
  return 1;
}

// ---------- команды ----------

static void cmd_sign(uint32_t idx, const char *hex, size_t hl) {
  static uint8_t raw[1536];
  static evm_tx_t tx;
  int n = mw_unhex(hex, hl, raw, sizeof raw);
  if (n <= 0) {
    mw_err("bad hex");
    return;
  }
  const char *e = evm_parse(raw, (size_t)n, &tx);
  if (e) {
    mw_err(e);
    return;
  }
  char to[43], num[80], amt[96], fee[96], nonce[24];
  eip55(tx.to, to);
  mw_fmt_units(tx.amount, 32, tx.token ? tx.token->decimals : 18, num, sizeof num);
  mw_cat(amt, sizeof amt, num, " ", tx.token ? tx.token->symbol : coin());
  mw_fmt_units(tx.max_fee, 32, 18, num, sizeof num);
  mw_cat(fee, sizeof fee, num, " ", coin());
  mw_fmt_u64(tx.nonce, 0, nonce, sizeof nonce);
  mw_amount("Amount", amt);
  mw_field("To", to);
  if (!mw_is_zero(tx.max_fee, 24) || be_u64(tx.max_fee) > EVM_HIGH_FEE) mw_warn("High fee!", "check it carefully");
  mw_amount("Max fee", fee);
  mw_field("Nonce", nonce);
  mw_field("Network", mw_net() ? EVM_NET_MAIN : EVM_NET_TEST);
  if (!mw_confirm(EVM_NAME " transfer")) {
    mw_err("rejected");
    return;
  }
  uint32_t p[5];
  uint8_t h[32], sig[65];
  char out[131];
  path_of(idx, p);
  mw_keccak256(raw, (size_t)n, h);  // тип 2 — вместе с байтом 0x02
  if (mw_sign(p, 5, h, sig)) {
    mw_err("signing failed");
    return;
  }
  mw_hex(sig, 65, out);
  mw_ok(out);
}

void mw_main(const char *cmd, size_t len) {
  const char *p = cmd, *end = cmd + len, *w, *iw;
  size_t wl = mw_word(&p, end, &w), il = mw_word(&p, end, &iw);
  uint32_t idx;
  char a[43];
  if (!wl || !mw_u32(iw, il, &idx) || idx > 0x7FFFFFFF) {
    mw_err("usage: addr|show|sign <i> ...");
    return;
  }
  if (wl == 4 && !memcmp(w, "sign", 4)) {
    const char *hx;
    size_t hl = mw_word(&p, end, &hx);
    cmd_sign(idx, hx, hl);
    return;
  }
  if (wl != 4 || (memcmp(w, "addr", 4) && memcmp(w, "show", 4))) {
    mw_err("unknown command");
    return;
  }
  if (!address(idx, a)) {
    mw_err("key error");
    return;
  }
  if (w[0] == 'a') {
    mw_ok(a);
    return;
  }
  char title[24], num[11];
  mw_u32str(idx, num);
  mw_cat(title, sizeof title, EVM_SHOW " #", num, 0);
  if (!mw_ui_address(title, mw_strlen(title), a, 42)) {
    mw_err("rejected");
    return;
  }
  mw_ok(a);
}
