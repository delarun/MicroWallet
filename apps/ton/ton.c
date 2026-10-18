// TON для MicroWallet: родная монета GRAM (до июня 2026 — Toncoin) и USDT, кошелёк W5 (v5R1).
//
// Ключи — как в TON-приложении Ledger: ed25519 по SLIP-10, m/44'/607'/<сеть>'/0'/<i>'/0'
// (сеть: 0 — mainnet, 1 — testnet). Адрес — хеш StateInit кошелька W5 с этим ключом.
//
// Подписывается хеш ячейки сообщения W5 (op "sign", wallet_id, срок, seqno, список действий).
// Хост присылает её в BoC; устройство само разбирает её целиком и строго:
//   ровно одно действие send_msg с режимом 3 (комиссия отдельно, ошибки игнорировать);
//   внутреннее сообщение без StateInit, src пустой, служебные поля нулевые;
//   тело — пусто, текстовый комментарий или перевод USDT (jetton transfer) на СВОЙ
//   кошелёк USDT (адрес устройство вычисляет само), сдача — обратно себе.
// Всё остальное (approve-подобные вызовы, плагины, несколько сообщений, «отправить весь
// баланс») — отказ.
//
// Команды: addr <i> | show <i> | pubkey <i> | sign <i> <BoC hex>  -> подпись ed25519 (64 байта)
#include "../../sdk/mw.h"

#define GRAM_DECIMALS 9
#define USDT_DECIMALS 6
#define MAX_CELLS 16
#define MEMO_MAX 96
#define HIGH_ATTACH 500000000ULL  // 0.5 GRAM на комиссию перевода жетона — уже подозрительно

#define OP_SIGNED_EXTERNAL 0x7369676eu
#define OP_SEND_MSG 0x0ec3c86du
#define OP_JETTON_TRANSFER 0x0f8a7ea5u
#define MODE_PAY_SEPARATELY_IGNORE_ERRORS 3

// Код W5R1 (@ton/ton WalletContractV5R1): хеш и глубина ячейки
static const uint8_t W5_CODE_HASH[32] = {
    0x20, 0x83, 0x4b, 0x7b, 0x72, 0xb1, 0x12, 0x14, 0x7e, 0x1b, 0x2f, 0xb4, 0x57, 0xb8, 0x4e, 0x74,
    0xd1, 0xa3, 0x0f, 0x04, 0xf7, 0x37, 0xd4, 0xf6, 0x2a, 0x66, 0x8e, 0x95, 0x52, 0xd2, 0xb7, 0x2f};
#define W5_CODE_DEPTH 6

// USDT (mainnet): мастер EQCxE6mU…-sDs; код кошелька — library-ячейка (её хеш, глубина 0)
static const uint8_t USDT_MASTER[32] = {
    0xb1, 0x13, 0xa9, 0x94, 0xb5, 0x02, 0x4a, 0x16, 0x71, 0x9f, 0x69, 0x13, 0x93, 0x28, 0xeb, 0x75,
    0x95, 0x96, 0xc3, 0x8a, 0x25, 0xf5, 0x90, 0x28, 0xb1, 0x46, 0xfe, 0xcd, 0xc3, 0x62, 0x1d, 0xfe};
static const uint8_t USDT_WALLET_LIB_HASH[32] = {
    0x89, 0x46, 0x8f, 0x02, 0xc7, 0x8e, 0x57, 0x08, 0x02, 0xe3, 0x99, 0x79, 0xc8, 0x51, 0x6f, 0xc3,
    0x8d, 0xf0, 0x7e, 0xa7, 0x6a, 0x48, 0x35, 0x7e, 0x05, 0x36, 0xf2, 0xba, 0x7b, 0x3e, 0xe3, 0x7b};

static int mainnet(void) { return mw_net(); }

// wallet_id W5: network_global_id (mainnet -239, testnet -3) XOR контекст клиента
// (1 бит «клиент», workchain 0, версия 0, подкошелёк 0) = 0x80000000
static uint32_t wallet_id(void) { return (uint32_t)(mainnet() ? -239 : -3) ^ 0x80000000u; }

static void path_of(uint32_t i, uint32_t p[6]) {
  p[0] = H(44);
  p[1] = H(607);
  p[2] = H(mainnet() ? 0 : 1);
  p[3] = H(0);
  p[4] = H(i);
  p[5] = H(0);
}

// ---------------- ячейки ----------------

typedef struct {
  uint8_t d[128];  // данные с меткой завершения, как в BoC
  uint16_t bits;
  uint8_t nref;
  uint8_t ref[4];  // индексы в g_cells (только у разобранных из BoC)
  uint8_t hash[32];
  uint16_t depth;
} cell_t;

// Хеш представления обычной ячейки уровня 0 (или exotic, если exotic=1)
static void repr_hash(const uint8_t *d, int bits, int nref, const uint8_t *const *rh, const uint16_t *rd,
                      int exotic, uint8_t out[32]) {
  mw_sha256_t c;
  uint8_t hdr[2] = {(uint8_t)(nref + (exotic ? 8 : 0)), (uint8_t)(bits / 8 + (bits + 7) / 8)};
  mw_sha256_init(&c);
  mw_sha256_update(&c, hdr, 2);
  mw_sha256_update(&c, d, (size_t)(bits + 7) / 8);  // последний байт уже с меткой 1000…
  for (int i = 0; i < nref; i++) {
    uint8_t dd[2] = {(uint8_t)(rd[i] >> 8), (uint8_t)rd[i]};
    mw_sha256_update(&c, dd, 2);
  }
  for (int i = 0; i < nref; i++) mw_sha256_update(&c, rh[i], 32);
  mw_sha256_final(&c, out);
}

// Сборка ячейки по битам (для адресов: данные кошелька, StateInit)
typedef struct {
  uint8_t d[128];
  int bits;
} bld_t;

static void put(bld_t *b, uint64_t v, int n) {
  for (int i = n - 1; i >= 0; i--, b->bits++) {
    if (!(b->bits & 7)) b->d[b->bits >> 3] = 0;
    if ((v >> i) & 1) b->d[b->bits >> 3] |= (uint8_t)(0x80 >> (b->bits & 7));
  }
}
static void put_bytes(bld_t *b, const uint8_t *p, int n) {
  for (int i = 0; i < n; i++) put(b, p[i], 8);
}
static void put_addr(bld_t *b, const uint8_t hash[32]) {  // addr_std$10, без anycast, workchain 0
  put(b, 2, 2);
  put(b, 0, 1);
  put(b, 0, 8);
  put_bytes(b, hash, 32);
}
static void finish(bld_t *b) {  // метка завершения, если биты не кратны 8
  if (b->bits & 7) b->d[b->bits >> 3] |= (uint8_t)(0x80 >> (b->bits & 7));
}

// Хеш StateInit{code, data}: биты 00110 (нет split_depth и special, есть code и data, нет библиотек)
static void state_init_hash(const uint8_t code_hash[32], uint16_t code_depth, const bld_t *data,
                            uint8_t out[32]) {
  uint8_t dh[32];
  repr_hash(data->d, data->bits, 0, 0, 0, 0, dh);
  bld_t si = {{0}, 0};
  put(&si, 0x06, 5);
  finish(&si);
  const uint8_t *rh[2] = {code_hash, dh};
  uint16_t rd[2] = {code_depth, 0};
  repr_hash(si.d, si.bits, 2, rh, rd, 0, out);
}

// Адрес своего кошелька W5 (хеш, workchain 0)
static int own_wallet(uint32_t idx, uint8_t addr[32], uint8_t pub[32]) {
  uint32_t p[6];
  path_of(idx, p);
  if (mw_ed25519_pubkey(p, 6, pub) != 0) return 0;
  bld_t d = {{0}, 0};
  put(&d, 1, 1);  // подпись разрешена
  put(&d, 0, 32);  // seqno
  put(&d, wallet_id(), 32);
  put_bytes(&d, pub, 32);
  put(&d, 0, 1);  // расширений нет
  finish(&d);
  state_init_hash(W5_CODE_HASH, W5_CODE_DEPTH, &d, addr);
  return 1;
}

// Адрес кошелька USDT владельца: StateInit(code = library, data = статус 0, баланс 0, owner, master)
static void usdt_wallet(const uint8_t owner[32], uint8_t out[32]) {
  bld_t d = {{0}, 0};
  put(&d, 0, 4);
  put(&d, 0, 4);  // Coins 0: длина 0
  put_addr(&d, owner);
  put_addr(&d, USDT_MASTER);
  finish(&d);
  state_init_hash(USDT_WALLET_LIB_HASH, 0, &d, out);
}

// ---------------- адрес для людей ----------------

static uint16_t crc16(const uint8_t *p, int n) {  // CRC-16/XMODEM
  uint16_t c = 0;
  for (int i = 0; i < n; i++) {
    c ^= (uint16_t)(p[i] << 8);
    for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
  }
  return c;
}

// base64url, 36 байт -> 48 символов. bounceable: EQ…/UQ… (testnet: kQ…/0Q…)
static void friendly(const uint8_t hash[32], int bounceable, char out[49]) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  uint8_t b[36];
  b[0] = (uint8_t)((bounceable ? 0x11 : 0x51) | (mainnet() ? 0 : 0x80));
  b[1] = 0;
  memcpy(b + 2, hash, 32);
  uint16_t c = crc16(b, 34);
  b[34] = (uint8_t)(c >> 8);
  b[35] = (uint8_t)c;
  for (int i = 0, o = 0; i < 36; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (uint32_t)b[i + 1] << 8 | b[i + 2];
    for (int k = 18; k >= 0; k -= 6) out[o++] = A[(v >> k) & 63];
  }
  out[48] = 0;
}

// ---------------- BoC ----------------

static cell_t g_cells[MAX_CELLS];
static int g_ncells;

static uint32_t crc32c(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
  }
  return ~c;
}

static uint64_t be(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = v << 8 | p[i];
  return v;
}

// Один корень, только обычные ячейки уровня 0; ссылки — только вперёд (топологический порядок)
static const char *boc_parse(const uint8_t *b, size_t n) {
  if (n < 6 || be(b, 4) != 0xb5ee9c72u) return "bad BoC";
  int has_idx = b[4] & 0x80, has_crc = b[4] & 0x40, s = b[4] & 7, off = b[5];
  if ((b[4] & 0x38) || s < 1 || s > 4 || off < 1 || off > 8) return "bad BoC";
  size_t p = 6;
  if (n < p + 3 * (size_t)s + off) return "bad BoC";
  uint64_t cells = be(b + p, s), roots = be(b + p + s, s), absent = be(b + p + 2 * s, s);
  p += 3 * (size_t)s;
  uint64_t total = be(b + p, off);
  p += off;
  if (cells < 1 || cells > MAX_CELLS || roots != 1 || absent) return "unsupported BoC";
  if (n < p + s || be(b + p, s) != 0) return "bad BoC";  // корень — ячейка 0
  p += s;
  if (has_idx) p += cells * off;
  if (has_crc) {
    if (n < 4 || crc32c(b, n - 4) != (uint32_t)(b[n - 4] | b[n - 3] << 8 | b[n - 2] << 16 | (uint32_t)b[n - 1] << 24))
      return "bad BoC checksum";
    n -= 4;
  }
  if (p > n || n - p != total) return "bad BoC";
  g_ncells = (int)cells;
  for (int i = 0; i < g_ncells; i++) {
    cell_t *c = &g_cells[i];
    if (p + 2 > n) return "bad BoC";
    int d1 = b[p], d2 = b[p + 1];
    p += 2;
    if (d1 & 0xF8) return "unsupported cell";  // exotic, уровни, хеши в BoC
    c->nref = (uint8_t)(d1 & 7);
    if (c->nref > 4) return "bad cell";
    int len = (d2 + 1) / 2;
    if (len > 128 || p + len + (size_t)c->nref * s > n) return "bad cell";
    memcpy(c->d, b + p, len);
    c->bits = (uint16_t)(d2 / 2 * 8);
    if (d2 & 1) {  // неполный байт: биты до метки 1000…
      uint8_t last = c->d[len - 1];
      if (!last) return "bad cell";
      int k = 0;
      while (!(last & (1 << k))) k++;
      c->bits = (uint16_t)((len - 1) * 8 + 7 - k);
    }
    p += len;
    for (int r = 0; r < c->nref; r++, p += s) {
      uint64_t x = be(b + p, s);
      if (x <= (uint64_t)i || x >= cells) return "bad cell refs";
      c->ref[r] = (uint8_t)x;
    }
  }
  if (p != n) return "bad BoC";
  for (int i = g_ncells - 1; i >= 0; i--) {  // хеши снизу вверх
    cell_t *c = &g_cells[i];
    const uint8_t *rh[4];
    uint16_t rd[4];
    c->depth = 0;
    for (int r = 0; r < c->nref; r++) {
      rh[r] = g_cells[c->ref[r]].hash;
      rd[r] = g_cells[c->ref[r]].depth;
      if (rd[r] + 1 > c->depth) c->depth = (uint16_t)(rd[r] + 1);
    }
    repr_hash(c->d, c->bits, c->nref, rh, rd, 0, c->hash);
  }
  return 0;
}

// ---------------- чтение ячейки ----------------

typedef struct {
  const cell_t *c;
  int pos, rpos;
  int err;
} sl_t;

static sl_t sl(const cell_t *c) { return (sl_t){c, 0, 0, 0}; }

static uint64_t ld(sl_t *s, int n) {
  uint64_t v = 0;
  if (s->pos + n > s->c->bits) {
    s->err = 1;
    return 0;
  }
  for (int i = 0; i < n; i++, s->pos++) v = v << 1 | ((s->c->d[s->pos >> 3] >> (7 - (s->pos & 7))) & 1);
  return v;
}
static const cell_t *ld_ref(sl_t *s) {
  if (s->rpos >= s->c->nref) {
    s->err = 1;
    return &g_cells[0];
  }
  return &g_cells[s->c->ref[s->rpos++]];
}
static int at_end(const sl_t *s) { return !s->err && s->pos == s->c->bits && s->rpos == s->c->nref; }
static int empty_cell(const cell_t *c) { return c->bits == 0 && c->nref == 0; }

// Coins (VarUInteger 16) -> 16 байт big-endian
static void ld_coins(sl_t *s, uint8_t v[16]) {
  int len = (int)ld(s, 4);
  memset(v, 0, 16);
  for (int i = 0; i < len; i++) v[16 - len + i] = (uint8_t)ld(s, 8);
}
static uint64_t coins_u64(const uint8_t v[16], int *ok) {
  if (!mw_is_zero(v, 8)) *ok = 0;
  return be(v + 8, 8);
}
// addr_std без anycast, только workchain 0
static int ld_addr(sl_t *s, uint8_t hash[32]) {
  if (ld(s, 2) != 2 || ld(s, 1) != 0 || ld(s, 8) != 0) return 0;
  for (int i = 0; i < 32; i++) hash[i] = (uint8_t)ld(s, 8);
  return !s->err;
}

// Текстовый комментарий: op 0 уже прочитан; остаток ячейки — печатный ASCII, без продолжений
static const char *ld_comment(sl_t *s, char out[MEMO_MAX + 1]) {
  int left = s->c->bits - s->pos;
  if (left % 8 || s->rpos != s->c->nref) return "unsupported comment";
  if (left / 8 > MEMO_MAX) return "comment too long to show";
  int n = left / 8;
  for (int i = 0; i < n; i++) {
    out[i] = (char)ld(s, 8);
    if (out[i] < 0x20 || out[i] > 0x7E) return "comment: only printable latin text";
  }
  out[n] = 0;
  return 0;
}

// ---------------- разбор подписываемого сообщения ----------------

typedef struct {
  uint8_t dest[32];  // получатель (у USDT — получатель жетона)
  int bounce;        // флаг для показа адреса (у USDT — неотскакивающий)
  int usdt;
  uint8_t amount[16];  // GRAM (нано) или USDT (1e-6)
  uint64_t attach;     // USDT: GRAM на комиссию (лишнее вернётся)
  char memo[MEMO_MAX + 1];
} ton_tx_t;

static const char *parse_body(sl_t *b, const uint8_t own[32], const uint8_t msg_dest[32], ton_tx_t *tx) {
  if (b->pos == b->c->bits && b->rpos == b->c->nref) return 0;  // без тела
  uint32_t op = (uint32_t)ld(b, 32);
  if (b->err) return "bad message body";
  if (op == 0) return ld_comment(b, tx->memo);
  if (op != OP_JETTON_TRANSFER) return "unsupported message body";
  if (!mainnet()) return "no tokens on testnet";
  uint8_t jw[32], resp[32], fwd[16];
  usdt_wallet(own, jw);
  if (memcmp(jw, msg_dest, 32)) return "not this wallet's USDT";
  tx->usdt = 1;
  ld(b, 64);  // query_id
  ld_coins(b, tx->amount);
  if (!ld_addr(b, tx->dest) || !ld_addr(b, resp)) return "bad jetton transfer";
  if (memcmp(resp, own, 32)) return "excess not returned to this wallet";
  if (ld(b, 1)) return "custom payload not supported";
  ld_coins(b, fwd);
  if (mw_is_zero(tx->amount, 16)) return "zero amount";
  const cell_t *pc = 0;
  sl_t fp = *b;
  if (ld(b, 1)) {  // forward_payload в ссылке
    pc = ld_ref(b);
    if (!at_end(b)) return "bad jetton transfer";
    fp = sl(pc);
  } else {
    fp = *b;
  }
  if (b->err) return "bad jetton transfer";
  if (fp.pos == fp.c->bits && fp.rpos == fp.c->nref) return 0;
  if (ld(&fp, 32) != 0 || fp.err) return "unsupported forward payload";
  return ld_comment(&fp, tx->memo);
}

static const char *ton_parse(const uint8_t own[32], ton_tx_t *tx) {
  sl_t r = sl(&g_cells[0]);
  if (ld(&r, 32) != OP_SIGNED_EXTERNAL) return "not a W5 transfer";
  if (ld(&r, 32) != wallet_id()) return "wrong network or wallet";
  ld(&r, 32);  // valid_until
  ld(&r, 32);  // seqno
  if (ld(&r, 1) != 1) return "no messages";
  const cell_t *list = ld_ref(&r);
  if (ld(&r, 1) != 0) return "extensions not supported";
  if (!at_end(&r)) return "bad transfer";

  sl_t a = sl(list);
  const cell_t *prev = ld_ref(&a);
  if (!empty_cell(prev)) return "only one message supported";
  if (ld(&a, 32) != OP_SEND_MSG) return "unsupported action";
  if (ld(&a, 8) != MODE_PAY_SEPARATELY_IGNORE_ERRORS) return "unsupported send mode";
  const cell_t *mc = ld_ref(&a);
  if (!at_end(&a)) return "bad action";

  sl_t m = sl(mc);
  uint8_t dest[32], value[16], z[16];
  if (ld(&m, 1) != 0) return "not an internal message";
  ld(&m, 1);  // ihr_disabled
  tx->bounce = (int)ld(&m, 1);
  if (ld(&m, 1) != 0 || ld(&m, 2) != 0) return "bad message";  // bounced, src = addr_none
  if (!ld_addr(&m, dest)) return "unsupported destination";
  ld_coins(&m, value);
  if (ld(&m, 1)) return "extra currencies not supported";
  ld_coins(&m, z);
  if (!mw_is_zero(z, 16)) return "bad message";
  ld_coins(&m, z);
  if (!mw_is_zero(z, 16) || ld(&m, 64) || ld(&m, 32)) return "bad message";
  if (ld(&m, 1)) return "state init not allowed";
  sl_t body = m;
  if (ld(&m, 1)) {
    body = sl(ld_ref(&m));
    if (!at_end(&m)) return "bad message";
  } else {
    body = m;
  }
  if (m.err || body.err) return "bad message";
  int ok = 1;
  uint64_t v = coins_u64(value, &ok);
  if (!ok) return "bad amount";
  memcpy(tx->dest, dest, 32);
  memcpy(tx->amount, value, 16);
  const char *e = parse_body(&body, own, dest, tx);
  if (e) return e;
  if (!tx->usdt && !at_end(&body)) return "bad message body";
  if (tx->usdt) {
    tx->attach = v;
    tx->bounce = 0;
  } else if (v == 0) {
    return "zero amount";
  }
  return 0;
}

// ---------------- команды ----------------

static void cmd_sign(uint32_t idx, const char *hex, size_t hl) {
  static uint8_t raw[1500];
  static ton_tx_t tx;
  uint8_t own[32], pub[32];
  int n = mw_unhex(hex, hl, raw, sizeof raw);
  const char *e = n < 0 ? "bad hex" : boc_parse(raw, (size_t)n);
  if (e) {
    mw_err(e);
    return;
  }
  if (!own_wallet(idx, own, pub)) {
    mw_err("bad path");
    return;
  }
  memset(&tx, 0, sizeof tx);
  if ((e = ton_parse(own, &tx))) {
    mw_err(e);
    return;
  }
  char to[49], num[64], amt[80];
  friendly(tx.dest, tx.bounce, to);
  mw_fmt_units(tx.amount, 16, tx.usdt ? USDT_DECIMALS : GRAM_DECIMALS, num, sizeof num);
  mw_cat(amt, sizeof amt, num, tx.usdt ? " USDT" : " GRAM", 0);
  mw_amount("Amount", amt);
  mw_field("To", to);
  if (tx.memo[0]) mw_field("Comment", tx.memo);
  if (tx.usdt) {  // GRAM на комиссию перевода жетона: лишнее вернётся
    if (tx.attach > HIGH_ATTACH) mw_warn("High fee!", "check it carefully");
    mw_fmt_u64(tx.attach, GRAM_DECIMALS, num, sizeof num);
    mw_cat(amt, sizeof amt, num, " GRAM", 0);
    mw_amount("Max fee", amt);
  }
  if (!mw_net()) mw_field("Network", "TON testnet");
  if (!mw_confirm("TON transfer")) {
    mw_err("rejected");
    return;
  }
  uint32_t p[6];
  uint8_t sig[64];
  char out[129];
  path_of(idx, p);
  if (mw_ed25519_sign(p, 6, g_cells[0].hash, 32, sig) != 0) {
    mw_err("sign failed");
    return;
  }
  mw_hex(sig, 64, out);
  mw_ok(out);
}

void mw_main(const char *cmd, size_t len) {
  const char *p = cmd, *end = cmd + len, *w, *iw;
  size_t wl = mw_word(&p, end, &w), il = mw_word(&p, end, &iw);
  uint32_t idx;
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
  uint8_t own[32], pub[32];
  char a[65];
  if (!own_wallet(idx, own, pub)) {
    mw_err("bad path");
    return;
  }
  if (wl == 6 && !memcmp(w, "pubkey", 6)) {  // хосту — для StateInit первой отправки
    mw_hex(pub, 32, a);
    mw_ok(a);
    return;
  }
  if (wl != 4 || (memcmp(w, "addr", 4) && memcmp(w, "show", 4))) {
    mw_err("unknown command");
    return;
  }
  friendly(own, 0, a);  // как в Tonkeeper: для получения — неотскакивающий UQ…
  if (w[0] == 'a') {
    mw_ok(a);
    return;
  }
  char title[24], num[11];
  mw_u32str(idx, num);
  mw_cat(title, sizeof title, "TON #", num, 0);
  if (!mw_ui_address(title, mw_strlen(title), a, mw_strlen(a))) {
    mw_err("rejected");
    return;
  }
  mw_ok(a);
}
