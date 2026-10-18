// BNB Chain для MicroWallet: адреса m/44'/60'/0'/0/i (как в MetaMask), перевод BNB и
// USDT-BEP20. Перенесено из встроенной монеты (src/evm.c до v0.5) без изменений.
//
// Подписываются только два вида legacy-транзакций EIP-155:
//   data пустая                       — перевод BNB на адрес to
//   to = токен из белого списка,
//   value = 0, data = transfer(a, n)  — перевод токена
// Всё остальное (вызовы контрактов, approve, деплой) отклоняется.
//
// Команды: addr <i> | show <i> | sign <i> <rlp hex>  -> r || s || recid
#include "mw.h"

#define HIGH_FEE_WEI 5000000000000000ULL  // 0.005 BNB

// mainnet: 0x55d398326f99059fF775485246999027B3197955; testnet: 0x337610d27c682E347C9cD60BD4b3b107C9d34dDd
static const uint8_t USDT_MAIN[20] = {0x55, 0xd3, 0x98, 0x32, 0x6f, 0x99, 0x05, 0x9f, 0xf7, 0x75,
                                      0x48, 0x52, 0x46, 0x99, 0x90, 0x27, 0xb3, 0x19, 0x79, 0x55};
static const uint8_t USDT_TEST[20] = {0x33, 0x76, 0x10, 0xd2, 0x7c, 0x68, 0x2e, 0x34, 0x7c, 0x9c,
                                      0xd6, 0x0b, 0xd4, 0xb3, 0xb1, 0x07, 0xc9, 0xd3, 0x4d, 0xdd};

static uint64_t chain_id(void) { return mw_net() ? 56 : 97; }
static const char *bnb(void) { return mw_net() ? "BNB" : "tBNB"; }

typedef struct {
  uint8_t to[20];
  int token;            // 1 — USDT (18 знаков), 0 — BNB
  uint8_t amount[32];   // big-endian
  uint8_t max_fee[32];  // gasPrice * gasLimit, wei
} evm_tx_t;

// ---------- RLP ----------

typedef struct {
  const uint8_t *p, *end;
} rlp_t;

// Строка RLP (не список) в каноничной форме
static int rlp_str(rlp_t *r, const uint8_t **d, size_t *n) {
  if (r->p >= r->end) return 0;
  uint8_t b = *r->p++;
  if (b < 0x80) {
    *d = r->p - 1;
    *n = 1;
    return 1;
  }
  if (b <= 0xB7) {
    *n = b - 0x80;
  } else if (b <= 0xBF) {
    size_t ll = b - 0xB7;
    if (ll > 2 || (size_t)(r->end - r->p) < ll || r->p[0] == 0) return 0;
    *n = 0;
    for (size_t i = 0; i < ll; i++) *n = (*n << 8) | *r->p++;
    if (*n < 56) return 0;
  } else {
    return 0;  // вложенный список
  }
  if ((size_t)(r->end - r->p) < *n) return 0;
  if (*n == 1 && r->p[0] < 0x80) return 0;  // должно было быть одним байтом
  *d = r->p;
  r->p += *n;
  return 1;
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

static const char *evm_parse(const uint8_t *buf, size_t len, evm_tx_t *tx) {
  static const uint8_t SEL[4] = {0xa9, 0x05, 0x9c, 0xbb};  // transfer(address,uint256)
  rlp_t r = {buf, buf + len};
  uint8_t num[32], gas_price[32], gas[32], value[32];
  const uint8_t *to, *data, *d;
  size_t to_n, data_n, n;
  memset(tx, 0, sizeof *tx);

  // Заголовок списка, который должен занимать весь буфер
  if (len < 1 || buf[0] < 0xC0) return "not an RLP list";
  size_t body;
  if (buf[0] <= 0xF7) {
    body = buf[0] - 0xC0;
    r.p += 1;
  } else {
    size_t ll = buf[0] - 0xF7;
    if (ll > 2 || len < 1 + ll || buf[1] == 0) return "bad RLP";
    body = 0;
    for (size_t i = 0; i < ll; i++) body = (body << 8) | buf[1 + i];
    if (body < 56) return "bad RLP";
    r.p += 1 + ll;
  }
  if ((size_t)(r.end - r.p) != body) return "bad RLP length";

  if (!rlp_uint(&r, 8, num)) return "bad nonce";
  if (!rlp_uint(&r, 8, gas_price)) return "bad gas price";
  if (!rlp_uint(&r, 4, gas)) return "bad gas limit";
  if (!rlp_str(&r, &to, &to_n) || to_n != 20) return "bad recipient";
  if (!rlp_uint(&r, 32, value)) return "bad value";
  if (!rlp_str(&r, &data, &data_n)) return "bad data";
  if (!rlp_uint(&r, 8, num) || be_u64(num) != chain_id()) return "wrong chain id";
  if (!rlp_str(&r, &d, &n) || n != 0) return "bad EIP-155 r";
  if (!rlp_str(&r, &d, &n) || n != 0) return "bad EIP-155 s";
  if (r.p != r.end) return "trailing bytes";

  uint8_t fee[16];
  mw_mul(gas_price + 24, 8, gas + 24, 8, fee);  // 64 x 64 -> 128 бит
  memcpy(tx->max_fee + 16, fee, 16);

  if (data_n == 0) {
    memcpy(tx->to, to, 20);
    memcpy(tx->amount, value, 32);
    return 0;
  }
  // a9059cbb | 12 нулей + адрес | uint256 сумма
  if (data_n != 68 || memcmp(data, SEL, 4) != 0) return "contract call not supported";
  if (!mw_is_zero(data + 4, 12)) return "bad token recipient";
  if (!mw_is_zero(value, 32)) return "BNB sent with token transfer";
  if (memcmp(to, mw_net() ? USDT_MAIN : USDT_TEST, 20)) return "unknown token";
  tx->token = 1;
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
  char to[43], num[80], amt[96], fee[96];
  eip55(tx.to, to);
  mw_fmt_units(tx.amount, 32, 18, num, sizeof num);
  mw_cat(amt, sizeof amt, num, " ", tx.token ? "USDT" : bnb());
  mw_fmt_units(tx.max_fee, 32, 18, num, sizeof num);
  mw_cat(fee, sizeof fee, num, " ", bnb());
  mw_amount("Amount", amt);
  mw_field("To", to);
  if (!mw_is_zero(tx.max_fee, 24) || be_u64(tx.max_fee) > HIGH_FEE_WEI) mw_warn("High fee!", "check it carefully");
  mw_amount("Max fee", fee);
  if (!mw_confirm("BSC transfer")) {
    mw_err("rejected");
    return;
  }
  uint32_t p[5];
  uint8_t h[32], sig[65];
  char out[131];
  path_of(idx, p);
  mw_keccak256(raw, (size_t)n, h);
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
  if (!mw_u32(iw, il, &idx) || idx > 0x7FFFFFFF) {
    mw_err("bad args");
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
    mw_err("bad path");
    return;
  }
  if (w[0] == 'a') {
    mw_ok(a);
    return;
  }
  char title[24], num[11];
  mw_u32str(idx, num);
  mw_cat(title, sizeof title, "BSC #", num, 0);
  if (!mw_ui_address(title, mw_strlen(title), a, 42)) {
    mw_err("rejected");
    return;
  }
  mw_ok(a);
}
