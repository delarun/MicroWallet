// Ethereum для MicroWallet: адреса m/44'/60'/0'/0/i (как в MetaMask), перевод ETH и
// токенов из белого списка. Транзакция — legacy EIP-155 (тип 0): хост присылает
// rlp([nonce, gasPrice, gas, to, value, data, chainId, 0, 0]), приложение само её
// разбирает, показывает и подписывает keccak256 от неё. Ответ: r || s || recid,
// v = chainId * 2 + 35 + recid считает хост.
//
// Команды: addr <i> | show <i> | sign <i> <rlp hex>
#include "mw.h"

typedef struct {
  uint8_t addr[20];
  const char *symbol;
  int decimals;
} token_t;

// mainnet: USDT, USDC; Sepolia: USDC от Circle
static const token_t MAINNET_TOKENS[] = {
    {{0xda, 0xc1, 0x7f, 0x95, 0x8d, 0x2e, 0xe5, 0x23, 0xa2, 0x20, 0x62, 0x06, 0x99, 0x45, 0x97, 0xc1, 0x3d, 0x83,
      0x1e, 0xc7},
     "USDT", 6},
    {{0xa0, 0xb8, 0x69, 0x91, 0xc6, 0x21, 0x8b, 0x36, 0xc1, 0xd1, 0x9d, 0x4a, 0x2e, 0x9e, 0xb0, 0xce, 0x36, 0x06,
      0xeb, 0x48},
     "USDC", 6},
};
static const token_t SEPOLIA_TOKENS[] = {
    {{0x1c, 0x7d, 0x4b, 0x19, 0x6c, 0xb0, 0xc7, 0xb0, 0x1d, 0x74, 0x3f, 0xbc, 0x61, 0x16, 0xa9, 0x02, 0x37, 0x9c,
      0x72, 0x38},
     "USDC", 6},
};

static void path_of(uint32_t i, uint32_t p[5]) {
  p[0] = H(44);
  p[1] = H(60);
  p[2] = H(0);
  p[3] = 0;
  p[4] = i;
}

// EIP-55: hex адреса, буква заглавная, если соответствующий полубайт keccak >= 8
static void eip55(const uint8_t a[20], char out[43]) {
  char low[41];
  uint8_t h[32];
  mw_hex(a, 20, low);
  mw_keccak256(low, 40, h);
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < 40; i++) {
    char c = low[i];
    int nib = (h[i / 2] >> (i % 2 ? 0 : 4)) & 15;
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

// ---------- RLP (только то, что нужно для legacy-транзакции) ----------

typedef struct {
  const uint8_t *d;
  size_t n;
} item_t;

static size_t be_len(const uint8_t *p, size_t n) {
  size_t v = 0;
  for (size_t i = 0; i < n; i++) v = v << 8 | p[i];
  return v;
}

// Элемент по смещению *off: строка (is_list = 0) или список
static int rlp_next(const uint8_t *p, size_t n, size_t *off, item_t *it, int *is_list) {
  if (*off >= n) return 0;
  uint8_t b = p[*off];
  size_t hdr, len;
  if (b < 0x80) {
    hdr = 0, len = 1, *is_list = 0;
  } else if (b <= 0xb7) {
    hdr = 1, len = b - 0x80, *is_list = 0;
  } else if (b <= 0xbf) {
    size_t ll = b - 0xb7;
    if (*off + 1 + ll > n || ll > 4) return 0;
    hdr = 1 + ll, len = be_len(p + *off + 1, ll), *is_list = 0;
  } else if (b <= 0xf7) {
    hdr = 1, len = b - 0xc0, *is_list = 1;
  } else {
    size_t ll = b - 0xf7;
    if (*off + 1 + ll > n || ll > 4) return 0;
    hdr = 1 + ll, len = be_len(p + *off + 1, ll), *is_list = 1;
  }
  if (*off + hdr + len > n) return 0;
  it->d = p + *off + hdr;
  it->n = len;
  *off += hdr + len;
  return 1;
}

static uint64_t u64(const item_t *it, int *ok) {
  uint64_t v = 0;
  if (it->n > 8) *ok = 0;
  for (size_t i = 0; i < it->n && i < 8; i++) v = v << 8 | it->d[i];
  return v;
}

// ---------- команды ----------

static void cmd_sign(uint32_t idx, const char *hex, size_t hl) {
  static uint8_t raw[1536];
  int n = mw_unhex(hex, hl, raw, sizeof raw);
  if (n <= 0) {
    mw_err("bad hex");
    return;
  }

  // rlp([nonce, gasPrice, gas, to, value, data, chainId, 0, 0])
  item_t list, f[9];
  size_t off = 0, o2 = 0;
  int is_list, k = 0, ok = 1;
  if (!rlp_next(raw, (size_t)n, &off, &list, &is_list) || !is_list || off != (size_t)n)
    {
    mw_err("not an rlp list");
    return;
  }
  while (k < 9 && rlp_next(list.d, list.n, &o2, &f[k], &is_list) && !is_list) k++;
  if (k != 9 || o2 != list.n) {
    mw_err("expected legacy transaction (9 fields)");
    return;
  }

  uint64_t chain = u64(&f[6], &ok), want = mw_net() ? 1 : 11155111;
  if (!ok || chain != want || f[7].n || f[8].n) {
    mw_err("wrong chain id");
    return;
  }
  if (f[3].n != 20) {
    mw_err("contract creation is not supported");
    return;
  }
  if (f[1].n > 32 || f[2].n > 8 || f[4].n > 32) {
    mw_err("bad numbers");
    return;
  }

  char amount[96], to[43], fee[96], num[80];
  const char *eth = mw_net() ? "ETH" : "SepoliaETH";
  if (f[5].n == 0) {  // перевод ETH
    mw_fmt_units(f[4].d, f[4].n, 18, num, sizeof num);
    mw_cat(amount, sizeof amount, num, " ", eth);
    eip55(f[3].d, to);
  } else {  // transfer(address,uint256) токена из белого списка
    const token_t *t = 0, *list_t = mw_net() ? MAINNET_TOKENS : SEPOLIA_TOKENS;
    size_t cnt = mw_net() ? sizeof MAINNET_TOKENS / sizeof *MAINNET_TOKENS : sizeof SEPOLIA_TOKENS / sizeof *SEPOLIA_TOKENS;
    for (size_t i = 0; i < cnt; i++)
      if (!memcmp(list_t[i].addr, f[3].d, 20)) t = &list_t[i];
    static const uint8_t SEL[4] = {0xa9, 0x05, 0x9c, 0xbb};
    if (!t) {
    mw_err("unknown contract: only whitelisted tokens");
    return;
  }
    if (f[5].n != 68 || memcmp(f[5].d, SEL, 4) || !mw_is_zero(f[5].d + 4, 12))
      {
    mw_err("only transfer(address,uint256) is supported");
    return;
  }
    if (!mw_is_zero(f[4].d, f[4].n)) {
    mw_err("token transfer must not send ETH");
    return;
  }
    mw_fmt_units(f[5].d + 36, 32, t->decimals, num, sizeof num);
    mw_cat(amount, sizeof amount, num, " ", t->symbol);
    eip55(f[5].d + 16, to);
  }
  uint8_t feeb[40];
  mw_mul(f[1].d, f[1].n, f[2].d, f[2].n, feeb);
  mw_fmt_units(feeb, f[1].n + f[2].n, 18, num, sizeof num);
  mw_cat(fee, sizeof fee, num, " ", eth);

  mw_field("Amount", amount);
  mw_field("To", to);
  mw_field("Max fee", fee);
  mw_field("Network", mw_net() ? "Ethereum" : "Sepolia testnet");
  if (!mw_confirm("Review transfer")) {
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
  const char *p = cmd, *end = cmd + len, *w;
  size_t wl = mw_word(&p, end, &w);
  const char *iw;
  size_t il = mw_word(&p, end, &iw);
  uint32_t idx;
  char a[43];
  if (!wl) {
    mw_err("usage: addr|show|sign <i> ...");
    return;
  }
  if (!mw_u32(iw, il, &idx) || idx > 0x7FFFFFFF) {
    mw_err("bad index");
    return;
  }

  if (wl == 4 && !memcmp(w, "addr", 4)) {
    if (!address(idx, a)) {
    mw_err("key error");
    return;
  }
    {
    mw_ok(a);
    return;
  }
  }
  if (wl == 4 && !memcmp(w, "show", 4)) {
    char title[24] = "Ethereum #", digits[11];
    int nd = 0;
    uint32_t v = idx;
    do digits[nd++] = (char)('0' + v % 10), v /= 10;
    while (v);
    size_t tl = 10;
    while (nd) title[tl++] = digits[--nd];
    if (!address(idx, a)) {
    mw_err("key error");
    return;
  }
    if (!mw_ui_address(title, tl, a, 42)) {
    mw_err("rejected");
    return;
  }
    {
    mw_ok(a);
    return;
  }
  }
  if (wl == 4 && !memcmp(w, "sign", 4)) {
    const char *hx;
    size_t hl = mw_word(&p, end, &hx);
    {
    cmd_sign(idx, hx, hl);
    return;
  }
  }
  mw_err("unknown command");
}
