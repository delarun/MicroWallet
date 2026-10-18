// utxo.c — общий код приложений Bitcoin (BIP84, P2WPKH) и Pearl (BIP86, Taproot).
// Подключается из apps/btc/btc.c (UTXO_PRL 0) и apps/prl/prl.c (UTXO_PRL 1).
// Перенесено из встроенных монет прошивки (src/tx.c, src/app.c до v0.5) без изменений
// поведения: те же адреса, тот же формат запроса и ответа, те же экраны.
//
// Команды:
//   addr [chain] <i>     адрес (chain 0 — приём, 1 — сдача)
//   show [chain] <i>     то же, со сверкой на экране
//   xpub                 (только Bitcoin) zpub / vpub аккаунта
//   sign <hex>           запрос (формат ниже) -> ok <подписи по входам>
//     BTC: "<DER+01>:<pubkey>" на вход (P2WPKH-свидетель), Pearl: BIP340-подпись на вход
//
// Формат запроса (числа little-endian):
//   u32 version, u32 locktime
//   u8  n_in;  на каждый вход:  txid[32] u32 vout u32 sequence u64 amount u8 chain u32 index
//   u8  n_out; на каждый выход: u64 amount u8 script_len script[] u8 is_change u32 change_index
#include "mw.h"

#define MAX_IN 16
#define MAX_OUT 8
#define MAX_SCRIPT 42
#define MAX_INDEX 0x7FFFFFFFu

#if UTXO_PRL
#define NAME "Pearl"
#define MAX_MONEY 2100000000000000000ULL  // 21e9 PRL в grain
#define HIGH_FEE 10000000ULL              // 0.1 PRL
#define PURPOSE 86
#else
#define NAME "Bitcoin"
#define MAX_MONEY 2100000000000000ULL  // 21e6 BTC в сатоши
#define HIGH_FEE 100000ULL             // 0.001 BTC
#define PURPOSE 84
#endif

static uint32_t coin_type(void) { return UTXO_PRL ? (mw_net() ? 808276 : 1) : (mw_net() ? 0 : 1); }
static const char *hrp(void) { return UTXO_PRL ? (mw_net() ? "prl" : "tprl") : (mw_net() ? "bc" : "tb"); }
static const char *unit(void) { return UTXO_PRL ? (mw_net() ? "PRL" : "tPRL") : (mw_net() ? "BTC" : "tBTC"); }

static void path_of(uint32_t chain, uint32_t idx, uint32_t p[5]) {
  p[0] = H(PURPOSE);
  p[1] = H(coin_type());
  p[2] = H(0);
  p[3] = chain;
  p[4] = idx;
}

// scriptPubKey своего адреса: BTC — P2WPKH (0014 hash160), Pearl — P2TR (5120 Q)
static int own_script(uint32_t chain, uint32_t idx, uint8_t s[34], uint8_t *n) {
  uint32_t p[5];
  path_of(chain, idx, p);
#if UTXO_PRL
  s[0] = 0x51;
  s[1] = 32;
  *n = 34;
  return mw_taproot_key(p, 5, s + 2) == 0;
#else
  uint8_t pub[65], c[33];
  if (mw_pubkey(p, 5, pub)) return 0;
  mw_compress(pub, c);
  s[0] = 0x00;
  s[1] = 20;
  *n = 22;
  return mw_hash160(c, 33, s + 2) == 0;
#endif
}

static int script_addr(const uint8_t *s, size_t n, char *out, size_t outlen) {
#if UTXO_PRL
  if (n != 34 || s[1] != 32) return 0;
  if (s[0] == 0x51) return mw_segwit_addr(out, outlen, hrp(), 1, s + 2, 32);  // P2TR
  if (s[0] == 0x52) return mw_segwit_addr(out, outlen, hrp(), 2, s + 2, 32);  // P2MR
  return 0;
#else
  if (n == 22 && s[0] == 0x00 && s[1] == 20) return mw_segwit_addr(out, outlen, hrp(), 0, s + 2, 20);  // P2WPKH
  if (n == 34 && s[0] == 0x00 && s[1] == 32) return mw_segwit_addr(out, outlen, hrp(), 0, s + 2, 32);  // P2WSH
  if (n == 34 && s[0] == 0x51 && s[1] == 32) return mw_segwit_addr(out, outlen, hrp(), 1, s + 2, 32);  // P2TR
  return 0;
#endif
}

static int address(uint32_t chain, uint32_t idx, char out[96]) {
  uint8_t s[34], n;
  return own_script(chain, idx, s, &n) && script_addr(s, n, out, 96);
}

// ---------- разбор запроса ----------

typedef struct {
  uint8_t txid[32];
  uint32_t vout, sequence;
  uint64_t amount;
  uint8_t chain;
  uint32_t index;
} tx_in_t;

typedef struct {
  uint64_t amount;
  uint8_t script_len;
  uint8_t script[MAX_SCRIPT];
  int is_change;
  uint32_t change_index;
} tx_out_t;

typedef struct {
  uint32_t version, locktime;
  uint8_t n_in, n_out;
  tx_in_t in[MAX_IN];
  tx_out_t out[MAX_OUT];
  uint64_t total_in, total_out, fee;
} tx_t;

typedef struct {
  const uint8_t *p;
  size_t left;
  int err;
} rd_t;

static void rdn(rd_t *r, uint8_t *o, size_t n) {
  if (r->err || r->left < n) {
    r->err = 1;
    memset(o, 0, n);
    return;
  }
  memcpy(o, r->p, n);
  r->p += n;
  r->left -= n;
}
static uint8_t rd8(rd_t *r) {
  uint8_t v;
  rdn(r, &v, 1);
  return v;
}
static uint32_t rd32(rd_t *r) {
  uint8_t b[4];
  rdn(r, b, 4);
  return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t rd64(rd_t *r) {
  uint64_t lo = rd32(r), hi = rd32(r);
  return lo | (hi << 32);
}

static const char *tx_parse(const uint8_t *buf, size_t len, tx_t *tx) {
  rd_t r = {buf, len, 0};
  memset(tx, 0, sizeof *tx);
  tx->version = rd32(&r);
  tx->locktime = rd32(&r);

  tx->n_in = rd8(&r);
  if (r.err) return "short";
  if (tx->n_in == 0 || tx->n_in > MAX_IN) return "bad input count";
  for (int i = 0; i < tx->n_in; i++) {
    tx_in_t *in = &tx->in[i];
    rdn(&r, in->txid, 32);
    in->vout = rd32(&r);
    in->sequence = rd32(&r);
    in->amount = rd64(&r);
    in->chain = rd8(&r);
    in->index = rd32(&r);
    if (r.err) return "short";
    if (in->chain > 1 || in->index > MAX_INDEX) return "bad input path";
    if (in->amount > MAX_MONEY) return "bad input amount";
    tx->total_in += in->amount;
    if (tx->total_in > MAX_MONEY) return "bad input total";
  }

  tx->n_out = rd8(&r);
  if (r.err) return "short";
  if (tx->n_out == 0 || tx->n_out > MAX_OUT) return "bad output count";
  for (int i = 0; i < tx->n_out; i++) {
    tx_out_t *o = &tx->out[i];
    o->amount = rd64(&r);
    o->script_len = rd8(&r);
    if (r.err) return "short";
    if (o->script_len > MAX_SCRIPT) return "script too long";
    rdn(&r, o->script, o->script_len);
    o->is_change = rd8(&r) != 0;
    o->change_index = rd32(&r);
    if (r.err) return "short";
    if (o->amount > MAX_MONEY) return "bad output amount";
    if (o->is_change) {
      // Не верим хосту на слово: выводим адрес сдачи сами и сравниваем
      uint8_t s[34], n;
      if (o->change_index > MAX_INDEX || !own_script(1, o->change_index, s, &n)) return "bad change path";
      if (o->script_len != n || memcmp(o->script, s, n)) return "change mismatch";
    } else {
      char a[96];
      if (!script_addr(o->script, o->script_len, a, sizeof a)) return "unsupported output script";
    }
    tx->total_out += o->amount;
    if (tx->total_out > MAX_MONEY) return "bad output total";
  }
  if (r.left) return "trailing bytes";
  if (tx->total_out > tx->total_in) return "outputs exceed inputs";
  tx->fee = tx->total_in - tx->total_out;
  return 0;
}

// ---------- sighash ----------

static void upd32(mw_sha256_t *c, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  mw_sha256_update(c, b, 4);
}
static void upd64(mw_sha256_t *c, uint64_t v) {
  upd32(c, (uint32_t)v);
  upd32(c, (uint32_t)(v >> 32));
}

#if UTXO_PRL
static void tagged_init(mw_sha256_t *c, const char *tag) {
  uint8_t h[32];
  mw_sha256(tag, mw_strlen(tag), h);
  mw_sha256_init(c);
  mw_sha256_update(c, h, 32);
  mw_sha256_update(c, h, 32);
}

// BIP341 key-path, SIGHASH_DEFAULT, без annex: подпись покрывает суммы и скрипты всех входов
static void sighash(const tx_t *tx, int idx, const uint8_t spk[][34], uint8_t out[32]) {
  mw_sha256_t c;
  uint8_t prevouts[32], amounts[32], scripts[32], sequences[32], outputs[32];
  static const uint8_t zero = 0, l34 = 34;

  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) {
    mw_sha256_update(&c, tx->in[i].txid, 32);
    upd32(&c, tx->in[i].vout);
  }
  mw_sha256_final(&c, prevouts);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) upd64(&c, tx->in[i].amount);
  mw_sha256_final(&c, amounts);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) {
    mw_sha256_update(&c, &l34, 1);
    mw_sha256_update(&c, spk[i], 34);
  }
  mw_sha256_final(&c, scripts);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) upd32(&c, tx->in[i].sequence);
  mw_sha256_final(&c, sequences);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_out; i++) {
    upd64(&c, tx->out[i].amount);
    mw_sha256_update(&c, &tx->out[i].script_len, 1);
    mw_sha256_update(&c, tx->out[i].script, tx->out[i].script_len);
  }
  mw_sha256_final(&c, outputs);

  tagged_init(&c, "TapSighash");
  mw_sha256_update(&c, &zero, 1);  // epoch
  mw_sha256_update(&c, &zero, 1);  // hash_type = SIGHASH_DEFAULT
  upd32(&c, tx->version);
  upd32(&c, tx->locktime);
  mw_sha256_update(&c, prevouts, 32);
  mw_sha256_update(&c, amounts, 32);
  mw_sha256_update(&c, scripts, 32);
  mw_sha256_update(&c, sequences, 32);
  mw_sha256_update(&c, outputs, 32);
  mw_sha256_update(&c, &zero, 1);  // spend_type: key-path
  upd32(&c, (uint32_t)idx);
  mw_sha256_final(&c, out);
}
#else
static void final_d(mw_sha256_t *c, uint8_t out[32]) {  // двойной SHA-256
  mw_sha256_final(c, out);
  mw_sha256(out, 32, out);
}

// BIP143 (SIGHASH_ALL) для P2WPKH-входа idx с публичным ключом pub
static void sighash(const tx_t *tx, int idx, const uint8_t pub[33], uint8_t out[32]) {
  mw_sha256_t c;
  uint8_t prevouts[32], sequences[32], outputs[32];

  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) {
    mw_sha256_update(&c, tx->in[i].txid, 32);
    upd32(&c, tx->in[i].vout);
  }
  final_d(&c, prevouts);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_in; i++) upd32(&c, tx->in[i].sequence);
  final_d(&c, sequences);
  mw_sha256_init(&c);
  for (int i = 0; i < tx->n_out; i++) {
    upd64(&c, tx->out[i].amount);
    mw_sha256_update(&c, &tx->out[i].script_len, 1);
    mw_sha256_update(&c, tx->out[i].script, tx->out[i].script_len);
  }
  final_d(&c, outputs);

  // scriptCode P2WPKH: OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG
  uint8_t sc[26] = {0x19, 0x76, 0xa9, 0x14};
  mw_hash160(pub, 33, sc + 4);
  sc[24] = 0x88;
  sc[25] = 0xac;

  const tx_in_t *in = &tx->in[idx];
  mw_sha256_init(&c);
  upd32(&c, tx->version);
  mw_sha256_update(&c, prevouts, 32);
  mw_sha256_update(&c, sequences, 32);
  mw_sha256_update(&c, in->txid, 32);
  upd32(&c, in->vout);
  mw_sha256_update(&c, sc, sizeof sc);
  upd64(&c, in->amount);
  upd32(&c, in->sequence);
  mw_sha256_update(&c, outputs, 32);
  upd32(&c, tx->locktime);
  upd32(&c, 1);  // SIGHASH_ALL
  final_d(&c, out);
}

// r||s -> DER (как ecdsa_sig_to_der в trezor-crypto)
static int der(const uint8_t sig[64], uint8_t out[72]) {
  int o = 2;
  for (int k = 0; k < 2; k++) {
    const uint8_t *v = sig + 32 * k;
    int i = 0;
    while (i < 31 && v[i] == 0) i++;
    int pad = v[i] >= 0x80, n = 32 - i;
    out[o++] = 0x02;
    out[o++] = (uint8_t)(n + pad);
    if (pad) out[o++] = 0;
    memcpy(out + o, v + i, n);
    o += n;
  }
  out[0] = 0x30;
  out[1] = (uint8_t)(o - 2);
  return o;
}
#endif

// ---------- экраны ----------

static int review(const tx_t *tx) {
  static char addr[MAX_OUT][96], amt[MAX_OUT][48], t_amt[MAX_OUT][16], t_to[MAX_OUT][16];
  char num[40], fee[48];
  int total = 0, k = 0;
  for (int i = 0; i < tx->n_out; i++) total += !tx->out[i].is_change;
  for (int i = 0; i < tx->n_out; i++) {
    const tx_out_t *o = &tx->out[i];
    if (o->is_change) continue;
    script_addr(o->script, o->script_len, addr[k], sizeof addr[k]);
    mw_fmt_u64(o->amount, 8, num, sizeof num);
    mw_cat(amt[k], sizeof amt[k], num, " ", unit());
    if (total > 1) {
      char a[11], b[11], ab[24];
      mw_u32str((uint32_t)k + 1, a);
      mw_u32str((uint32_t)total, b);
      mw_cat(ab, sizeof ab, a, "/", b);
      mw_cat(t_amt[k], sizeof t_amt[k], "Amount ", ab, 0);
      mw_cat(t_to[k], sizeof t_to[k], "To ", ab, 0);
    } else {
      mw_cat(t_amt[k], sizeof t_amt[k], "Amount", 0, 0);
      mw_cat(t_to[k], sizeof t_to[k], "To", 0, 0);
    }
    mw_amount(t_amt[k], amt[k]);
    mw_field(t_to[k], addr[k]);
    k++;
  }
  // Грубая защита от ошибок хоста: комиссия больше 10% или выше порога
  if (tx->fee > HIGH_FEE || (tx->total_out && tx->fee * 10 > tx->total_out)) mw_warn("High fee!", "check it carefully");
  mw_fmt_u64(tx->fee, 8, num, sizeof num);
  mw_cat(fee, sizeof fee, num, " ", unit());
  mw_amount("Fee", fee);
  return mw_confirm(UTXO_PRL ? "Pearl transfer" : "Bitcoin transfer");
}

// ---------- команды ----------

static void cmd_sign(const char *hex, size_t hl) {
  static uint8_t req[MAX_IN * 60 + MAX_OUT * 60 + 16];
  static tx_t tx;
  static char out[MAX_IN * 220 + 8];
  int n = mw_unhex(hex, hl, req, sizeof req);
  if (n <= 0) {
    mw_err("bad hex");
    return;
  }
  const char *e = tx_parse(req, (size_t)n, &tx);
  if (e) {
    mw_err(e);
    return;
  }
  if (!review(&tx)) {
    mw_err("rejected");
    return;
  }
  size_t pos = 0;
#if UTXO_PRL
  static uint8_t spk[MAX_IN][34];
  for (int i = 0; i < tx.n_in; i++) {
    uint8_t sn;
    if (!own_script(tx.in[i].chain, tx.in[i].index, spk[i], &sn)) {
      mw_err("key error");
      return;
    }
  }
  for (int i = 0; i < tx.n_in; i++) {
    uint32_t p[5];
    uint8_t h[32], sig[64];
    path_of(tx.in[i].chain, tx.in[i].index, p);
    sighash(&tx, i, (const uint8_t(*)[34])spk, h);
    if (mw_taproot_sign(p, 5, h, sig)) {
      mw_err("signing failed");
      return;
    }
    if (pos) out[pos++] = ' ';
    mw_hex(sig, 64, out + pos);
    pos += 128;
  }
#else
  for (int i = 0; i < tx.n_in; i++) {
    uint32_t p[5];
    uint8_t pub[65], c[33], h[32], sig[65], d[73];
    path_of(tx.in[i].chain, tx.in[i].index, p);
    if (mw_pubkey(p, 5, pub)) {
      mw_err("key error");
      return;
    }
    mw_compress(pub, c);
    sighash(&tx, i, c, h);
    if (mw_sign(p, 5, h, sig)) {
      mw_err("signing failed");
      return;
    }
    int dn = der(sig, d);
    d[dn++] = 0x01;  // SIGHASH_ALL
    if (pos) out[pos++] = ' ';
    mw_hex(d, (size_t)dn, out + pos);
    pos += 2 * (size_t)dn;
    out[pos++] = ':';
    mw_hex(c, 33, out + pos);
    pos += 66;
  }
#endif
  out[pos] = 0;
  mw_reply(out, pos);
}

// "<i>" или "<chain> <i>"
static int parse_path(const char **p, const char *end, uint32_t *chain, uint32_t *idx) {
  const char *a, *b;
  size_t al = mw_word(p, end, &a), bl = mw_word(p, end, &b);
  if (!bl) {
    *chain = 0;
    return mw_u32(a, al, idx) && *idx <= MAX_INDEX;
  }
  return mw_u32(a, al, chain) && *chain <= 1 && mw_u32(b, bl, idx) && *idx <= MAX_INDEX;
}

void mw_main(const char *cmd, size_t len) {
  const char *p = cmd, *end = cmd + len, *w;
  size_t wl = mw_word(&p, end, &w);
  uint32_t chain, idx;
  char a[96];

  if (wl == 4 && !memcmp(w, "addr", 4)) {
    if (!parse_path(&p, end, &chain, &idx)) {
      mw_err("bad path");
      return;
    }
    if (!address(chain, idx, a)) {
      mw_err("bad path");
      return;
    }
    mw_ok(a);
    return;
  }
  if (wl == 4 && !memcmp(w, "show", 4)) {
    char title[32], num[11];
    if (!parse_path(&p, end, &chain, &idx) || !address(chain, idx, a)) {
      mw_err("bad path");
      return;
    }
    mw_u32str(idx, num);
    mw_cat(title, sizeof title, NAME, chain ? " chg #" : " #", num);
    if (!mw_ui_address(title, mw_strlen(title), a, mw_strlen(a))) {
      mw_err("rejected");
      return;
    }
    mw_ok(a);
    return;
  }
  if (wl == 4 && !memcmp(w, "sign", 4)) {
    const char *hx;
    size_t hl = mw_word(&p, end, &hx);
    cmd_sign(hx, hl);
    return;
  }
#if !UTXO_PRL
  if (wl == 4 && !memcmp(w, "xpub", 4)) {
    uint32_t path[3] = {H(84), H(coin_type()), H(0)};
    char x[120];
    // zpub (mainnet) / vpub (testnet) — SLIP-132 для BIP84
    if (mw_xpub(path, 3, mw_net() ? 0x04B24746u : 0x045F1CF6u, x, sizeof x) <= 0) {
      mw_err("key error");
      return;
    }
    mw_ok(x);
    return;
  }
#endif
  mw_err("unknown command");
}
