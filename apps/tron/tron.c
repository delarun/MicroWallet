// TRON для MicroWallet: адреса m/44'/195'/0'/0/i (как в TronLink), перевод TRX и
// USDT-TRC20. Перенесено из встроенной монеты (src/tron.c до v0.5) без изменений.
//
// Подписывается raw_data, как его собрала нода: устройство само разбирает protobuf.
// Поддерживается ровно один контракт:
//   TransferContract (type 1)       — перевод TRX
//   TriggerSmartContract (type 31)  — transfer(address,uint256) токена из белого списка
// Protobuf допускает повторы полей, поэтому любое неизвестное или повторное поле —
// ошибка: устройство и нода должны видеть одну и ту же транзакцию.
//
// Команды: addr <i> | show <i> | sign <i> <raw_data hex>  -> r || s || recid
#include "mw.h"

#define TRX_DECIMALS 6
#define HIGH_FEE_SUN 100000000ULL  // 100 TRX

typedef struct {
  const char *symbol;
  int decimals;
  uint8_t addr[20];  // без префикса 0x41
} token_t;

// mainnet: TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t; Nile: TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf
static const token_t MAINNET_USDT = {"USDT", 6, {0xa6, 0x14, 0xf8, 0x03, 0xb6, 0xfd, 0x78, 0x09, 0x86, 0xa4,
                                                 0x2c, 0x78, 0xec, 0x9c, 0x7f, 0x77, 0xe6, 0xde, 0xd1, 0x3c}};
static const token_t NILE_USDT = {"USDT", 6, {0xec, 0xa9, 0xbc, 0x82, 0x8a, 0x30, 0x05, 0xb9, 0xa3, 0xb9,
                                              0x09, 0xf2, 0xcc, 0x5c, 0x2a, 0x54, 0x79, 0x4d, 0xe0, 0x5f}};

static const token_t *find_token(const uint8_t a[20]) {
  const token_t *t = mw_net() ? &MAINNET_USDT : &NILE_USDT;
  return memcmp(t->addr, a, 20) ? 0 : t;
}

typedef struct {
  uint8_t owner[20];
  uint8_t to[20];
  const token_t *token;  // 0 — перевод TRX
  uint8_t amount[32];    // big-endian
  uint64_t fee_limit;    // sun, только TRC20
} tron_tx_t;

// ---------- protobuf ----------

typedef struct {
  const uint8_t *p, *end;
} pb_t;

typedef struct {
  uint32_t num, wt;
  uint64_t v;
  const uint8_t *d;
  size_t dlen;
} pb_field_t;

static int pb_varint(pb_t *r, uint64_t *v) {
  *v = 0;
  for (int sh = 0; sh < 64 && r->p < r->end; sh += 7) {
    uint8_t b = *r->p++;
    if (sh == 63 && b > 1) return 0;
    *v |= (uint64_t)(b & 0x7F) << sh;
    if (!(b & 0x80)) return 1;
  }
  return 0;
}

// 1 = поле прочитано, 0 = конец, -1 = ошибка
static int pb_next(pb_t *r, pb_field_t *f, uint32_t *seen) {
  if (r->p == r->end) return 0;
  uint64_t key;
  if (!pb_varint(r, &key)) return -1;
  f->num = (uint32_t)(key >> 3);
  f->wt = key & 7;
  if (key >> 3 >= 32 || f->num == 0) return -1;
  if (*seen & (1u << f->num)) return -1;  // повтор
  *seen |= 1u << f->num;
  if (f->wt == 0) return pb_varint(r, &f->v) ? 1 : -1;
  if (f->wt == 2) {
    uint64_t n;
    if (!pb_varint(r, &n) || n > (uint64_t)(r->end - r->p)) return -1;
    f->d = r->p;
    f->dlen = (size_t)n;
    r->p += n;
    return 1;
  }
  return -1;
}

#define FOR_FIELDS(buf, len, f, rc) \
  pb_t _r = {(buf), (buf) + (len)}; \
  uint32_t _seen = 0;               \
  pb_field_t f;                     \
  int rc;                           \
  while ((rc = pb_next(&_r, &f, &_seen)) == 1)

static int tron_addr(const pb_field_t *f, uint8_t out[20]) {
  if (f->wt != 2 || f->dlen != 21 || f->d[0] != 0x41) return 0;
  memcpy(out, f->d + 1, 20);
  return 1;
}

static const char *parse_transfer(const uint8_t *b, size_t n, tron_tx_t *tx) {
  uint32_t have = 0;
  FOR_FIELDS(b, n, f, rc) {
    if (f.num == 1 && tron_addr(&f, tx->owner)) have |= 1;
    else if (f.num == 2 && tron_addr(&f, tx->to)) have |= 2;
    else if (f.num == 3 && f.wt == 0) {
      if (f.v == 0 || f.v >> 63) return "bad amount";
      for (int i = 0; i < 8; i++) tx->amount[31 - i] = (uint8_t)(f.v >> (8 * i));
      have |= 4;
    } else return "unsupported transfer field";
  }
  if (rc < 0) return "bad protobuf";
  return have == 7 ? 0 : "incomplete transfer";
}

static const char *parse_trigger(const uint8_t *b, size_t n, tron_tx_t *tx) {
  static const uint8_t SEL[4] = {0xa9, 0x05, 0x9c, 0xbb};  // transfer(address,uint256)
  uint8_t contract[20];
  uint32_t have = 0;
  FOR_FIELDS(b, n, f, rc) {
    if (f.num == 1 && tron_addr(&f, tx->owner)) have |= 1;
    else if (f.num == 2 && tron_addr(&f, contract)) have |= 2;
    else if ((f.num == 3 || f.num == 5 || f.num == 6) && f.wt == 0) {
      if (f.v) return "call with TRX/TRC10 value";
    } else if (f.num == 4 && f.wt == 2) {
      // a9059cbb | 12 нулей + адрес | uint256 сумма
      if (f.dlen != 68 || memcmp(f.d, SEL, 4) != 0) return "not a token transfer";
      if (!mw_is_zero(f.d + 4, 12)) return "bad recipient";
      memcpy(tx->to, f.d + 16, 20);
      memcpy(tx->amount, f.d + 36, 32);
      have |= 4;
    } else return "unsupported contract field";
  }
  if (rc < 0) return "bad protobuf";
  if (have != 7) return "incomplete contract call";
  tx->token = find_token(contract);
  return tx->token ? 0 : "unknown token";
}

static int str_eq(const pb_field_t *f, const char *s) {
  return f->wt == 2 && f->dlen == mw_strlen(s) && memcmp(f->d, s, f->dlen) == 0;
}

static const char *parse_contract(const uint8_t *b, size_t n, tron_tx_t *tx) {
  static const char TRANSFER[] = "type.googleapis.com/protocol.TransferContract";
  static const char TRIGGER[] = "type.googleapis.com/protocol.TriggerSmartContract";
  uint64_t type = 0;
  pb_field_t url = {0}, val = {0};
  FOR_FIELDS(b, n, f, rc) {
    if (f.num == 1 && f.wt == 0) {
      type = f.v;
    } else if (f.num == 2 && f.wt == 2) {  // google.protobuf.Any {type_url, value}
      FOR_FIELDS(f.d, f.dlen, a, rc2) {
        if (a.num == 1) url = a;
        else if (a.num == 2) val = a;
        else return "bad parameter";
      }
      if (rc2 < 0) return "bad protobuf";
    } else if (f.num == 5 && f.wt == 0) {  // Permission_id
      if (f.v) return "multisig permission not supported";
    } else {
      return "unsupported contract field";
    }
  }
  if (rc < 0) return "bad protobuf";
  if (val.wt != 2 || !val.d) return "no parameter";
  if (type == 1 && str_eq(&url, TRANSFER)) return parse_transfer(val.d, val.dlen, tx);
  if (type == 31 && str_eq(&url, TRIGGER)) return parse_trigger(val.d, val.dlen, tx);
  return "unsupported contract type";
}

static const char *tron_parse(const uint8_t *raw, size_t len, tron_tx_t *tx) {
  int contracts = 0;
  const char *e = 0;
  memset(tx, 0, sizeof *tx);
  FOR_FIELDS(raw, len, f, rc) {
    switch (f.num) {
      case 1:  // ref_block_bytes
      case 4:  // ref_block_hash
        if (f.wt != 2) return "bad protobuf";
        break;
      case 3:   // ref_block_num
      case 8:   // expiration
      case 14:  // timestamp
        if (f.wt != 0) return "bad protobuf";
        break;
      case 18:  // fee_limit
        if (f.wt != 0 || f.v >> 63) return "bad fee limit";
        tx->fee_limit = f.v;
        break;
      case 11:  // contract
        if (f.wt != 2) return "bad protobuf";
        contracts++;
        if ((e = parse_contract(f.d, f.dlen, tx))) return e;
        break;
      default:  // auths, data (memo), scripts ...
        return "unsupported tx field";
    }
  }
  if (rc < 0) return "bad protobuf";
  if (contracts != 1) return "need exactly one contract";
  if (!tx->token && tx->fee_limit) return "fee limit on TRX transfer";
  return 0;
}

// ---------- адреса ----------

static void path_of(uint32_t i, uint32_t p[5]) {
  p[0] = H(44);
  p[1] = H(195);
  p[2] = H(0);
  p[3] = 0;
  p[4] = i;
}

static int account_id(uint32_t i, uint8_t id[20]) {
  uint32_t p[5];
  uint8_t pub[65], h[32];
  path_of(i, p);
  if (mw_pubkey(p, 5, pub)) return 0;
  mw_keccak256(pub + 1, 64, h);
  memcpy(id, h + 12, 20);
  return 1;
}

static int encode(const uint8_t id[20], char out[40]) {
  uint8_t b[21] = {0x41};
  memcpy(b + 1, id, 20);
  return mw_base58check(b, 21, out, 40);
}

// ---------- команды ----------

static void cmd_sign(uint32_t idx, const char *hex, size_t hl) {
  static uint8_t raw[1536];
  static tron_tx_t tx;
  uint8_t own[20];
  int n = mw_unhex(hex, hl, raw, sizeof raw);
  if (n <= 0) {
    mw_err("bad hex");
    return;
  }
  const char *e = tron_parse(raw, (size_t)n, &tx);
  if (!e && (!account_id(idx, own) || memcmp(own, tx.owner, 20))) e = "not our address";
  if (e) {
    mw_err(e);
    return;
  }
  char to[40], num[80], amt[96], fee[48];
  encode(tx.to, to);
  mw_fmt_units(tx.amount, 32, tx.token ? tx.token->decimals : TRX_DECIMALS, num, sizeof num);
  mw_cat(amt, sizeof amt, num, " ", tx.token ? tx.token->symbol : "TRX");
  mw_amount("Amount", amt);
  mw_field("To", to);
  if (tx.token) {  // TRC20: TRX сжигается за энергию, fee_limit — верхняя граница
    if (tx.fee_limit > HIGH_FEE_SUN) mw_warn("High fee!", "check it carefully");
    mw_fmt_u64(tx.fee_limit, TRX_DECIMALS, num, sizeof num);
    mw_cat(fee, sizeof fee, num, " TRX", 0);
    mw_amount("Max fee", fee);
  }
  if (!mw_confirm("TRON transfer")) {
    mw_err("rejected");
    return;
  }
  uint32_t p[5];
  uint8_t h[32], sig[65];
  char out[131];
  path_of(idx, p);
  mw_sha256(raw, (size_t)n, h);  // txID
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
  uint8_t id[20];
  char a[40];
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
  if (!account_id(idx, id) || !encode(id, a)) {
    mw_err("bad path");
    return;
  }
  if (w[0] == 'a') {
    mw_ok(a);
    return;
  }
  char title[24], num[11];
  mw_u32str(idx, num);
  mw_cat(title, sizeof title, "TRON #", num, 0);
  if (!mw_ui_address(title, mw_strlen(title), a, mw_strlen(a))) {
    mw_err("rejected");
    return;
  }
  mw_ok(a);
}
