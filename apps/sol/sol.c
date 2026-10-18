// Solana для MicroWallet: перевод SOL и токенов USDC/USDT (SPL). Ключи — как в Phantom:
// ed25519 по SLIP-10, m/44'/501'/<i>'/0'; адрес — base58 открытого ключа.
//
// Подписывается сообщение транзакции целиком (legacy или v0), как его собрал хост. Устройство
// само разбирает его и строго:
//   одна подпись — своя (плательщик комиссии — этот кошелёк), таблиц адресов (v0) нет;
//   ровно один перевод: System transfer со своего адреса ИЛИ Token TransferChecked токена
//   из белого списка (точность — как в списке) со своей подписью;
//   для токена — не больше одного CreateIdempotent (ATA получателя), его платит этот кошелёк;
//   кроме этого — не больше одного SetComputeUnitLimit и одного SetComputeUnitPrice.
// Всё остальное (другие программы, assign, createAccount, второй перевод) — отказ.
// Счёт токена получателя — ATA(владелец, mint): хост сообщает владельца (его адрес и видит
// человек) и bump, устройство само считает адрес PDA и сверяет с транзакцией. Другой bump
// даёт адрес, на котором токен-счёта быть не может: перевод не пройдёт, а не уйдёт не туда.
// Комиссия на экране — потолок: 5000 lamports за подпись + цена × лимит вычислений.
//
// Команды: addr <i> | show <i> | sign <i> <message hex> [владелец base58 bump] -> подпись (64 байта)
#include "../../sdk/mw.h"

#define SOL_DECIMALS 9
#define BASE_FEE 5000ULL           // lamports за подпись
#define DEFAULT_CU 200000ULL       // лимит вычислений на инструкцию, если не задан
#define HIGH_FEE 10000000ULL       // 0.01 SOL
#define MAX_KEYS 16
#define MAX_IX 4
#define TOKEN_ACCOUNT_RENT 2039280ULL  // lamports: рента токен-счёта (165 байт) — платит создающий

static const uint8_t SYSTEM[32] = {0};
static const uint8_t COMPUTE_BUDGET[32] = {
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, 0xff, 0xec, 0xad, 0xba, 0x72, 0xc3, 0x9b, 0xe7,
    0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b, 0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00};

static const uint8_t TOKEN[32] = {  // TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9};
static const uint8_t ATA[32] = {  // ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1, 0xbb, 0x3d, 0x10, 0x29, 0x14, 0x8e, 0x0d, 0x83,
    0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84, 0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59};

typedef struct {
  uint8_t mint[32];
  const char *symbol;
  int decimals;
} token_t;

// Белый список (mint сверены с сетью: программа Token, точность 6)
static const token_t TOKENS_MAIN[] = {
    {{0xc6, 0xfa, 0x7a, 0xf3, 0xbe, 0xdb, 0xad, 0x3a, 0x3d, 0x65, 0xf3, 0x6a, 0xab, 0xc9, 0x74, 0x31,
      0xb1, 0xbb, 0xe4, 0xc2, 0xd2, 0xf6, 0xe0, 0xe4, 0x7c, 0xa6, 0x02, 0x03, 0x45, 0x2f, 0x5d, 0x61},
     "USDC", 6},  // EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v
    {{0xce, 0x01, 0x0e, 0x60, 0xaf, 0xed, 0xb2, 0x27, 0x17, 0xbd, 0x63, 0x19, 0x2f, 0x54, 0x14, 0x5a,
      0x3f, 0x96, 0x5a, 0x33, 0xbb, 0x82, 0xd2, 0xc7, 0x02, 0x9e, 0xb2, 0xce, 0x1e, 0x20, 0x82, 0x64},
     "USDT", 6},  // Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB
};
static const token_t TOKENS_TEST[] = {
    {{0x3b, 0x44, 0x2c, 0xb3, 0x91, 0x21, 0x57, 0xf1, 0x3a, 0x93, 0x3d, 0x01, 0x34, 0x28, 0x2d, 0x03,
      0x2b, 0x5f, 0xfe, 0xcd, 0x01, 0xa2, 0xdb, 0xf1, 0xb7, 0x79, 0x06, 0x08, 0xdf, 0x00, 0x2e, 0xa7},
     "USDC", 6},  // 4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU (Circle, devnet)
};

static const token_t *find_token(const uint8_t mint[32]) {
  const token_t *t = mw_net() ? TOKENS_MAIN : TOKENS_TEST;
  int n = mw_net() ? (int)(sizeof TOKENS_MAIN / sizeof *TOKENS_MAIN) : (int)(sizeof TOKENS_TEST / sizeof *TOKENS_TEST);
  for (int i = 0; i < n; i++)
    if (!memcmp(t[i].mint, mint, 32)) return &t[i];
  return 0;
}

// ATA(владелец, mint) с данным bump: sha256(owner || Token || mint || bump || ATA || "ProgramDerivedAddress")
static void ata_address(const uint8_t owner[32], const uint8_t mint[32], uint8_t bump, uint8_t out[32]) {
  static const char TAG[] = "ProgramDerivedAddress";
  uint8_t b[32 * 4 + 1 + sizeof TAG - 1];
  memcpy(b, owner, 32);
  memcpy(b + 32, TOKEN, 32);
  memcpy(b + 64, mint, 32);
  b[96] = bump;
  memcpy(b + 97, ATA, 32);
  memcpy(b + 129, TAG, sizeof TAG - 1);
  mw_sha256(b, sizeof b, out);
}

static void path_of(uint32_t i, uint32_t p[4]) {
  p[0] = H(44);
  p[1] = H(501);
  p[2] = H(i);
  p[3] = H(0);
}

// base58 без контрольной суммы (адреса и ключи Solana)
static void b58(const uint8_t *in, int n, char *out) {
  static const char A[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t d[64];
  int len = 0, zeros = 0;
  while (zeros < n && !in[zeros]) zeros++;
  for (int i = zeros; i < n; i++) {
    int carry = in[i];
    for (int j = 0; j < len; j++) {
      carry += d[j] << 8;
      d[j] = (uint8_t)(carry % 58);
      carry /= 58;
    }
    while (carry) {
      d[len++] = (uint8_t)(carry % 58);
      carry /= 58;
    }
  }
  int o = 0;
  for (int i = 0; i < zeros; i++) out[o++] = '1';
  for (int i = len - 1; i >= 0; i--) out[o++] = A[d[i]];
  out[o] = 0;
}

// base58 -> ровно 32 байта; 0 — не адрес
static int b58dec32(const char *s, size_t n, uint8_t out[32]) {
  static const char A[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t b[40] = {0};  // big-endian, с запасом
  int zeros = 0;
  if (n < 32 || n > 44) return 0;
  while ((size_t)zeros < n && s[zeros] == '1') zeros++;
  for (size_t i = 0; i < n; i++) {
    const char *c = A;
    while (*c && *c != s[i]) c++;
    if (!*c) return 0;
    int carry = (int)(c - A);
    for (int j = 39; j >= 0; j--) {
      carry += b[j] * 58;
      b[j] = (uint8_t)carry;
      carry >>= 8;
    }
    if (carry) return 0;
  }
  int lead = 0;
  while (lead < 40 && !b[lead]) lead++;
  if (40 - lead + zeros != 32) return 0;
  memset(out, 0, 32);
  memcpy(out + zeros, b + lead, 40 - lead);
  return 1;
}

// ---------------- разбор сообщения ----------------

typedef struct {
  const uint8_t *p, *end;
  int err;
} rd_t;

static uint32_t u8(rd_t *r) {
  if (r->p >= r->end) {
    r->err = 1;
    return 0;
  }
  return *r->p++;
}
static uint64_t le(rd_t *r, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= (uint64_t)u8(r) << (8 * i);
  return v;
}
static uint32_t compact(rd_t *r) {  // compact-u16
  uint32_t v = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t b = u8(r);
    v |= (b & 0x7f) << (7 * i);
    if (!(b & 0x80)) return v;
  }
  r->err = 1;
  return 0;
}
static const uint8_t *take(rd_t *r, uint32_t n) {
  if ((uint32_t)(r->end - r->p) < n) {
    r->err = 1;
    return 0;
  }
  const uint8_t *q = r->p;
  r->p += n;
  return q;
}

typedef struct {
  uint8_t to[32];       // SOL — получатель; токен — владелец счёта получателя (кошелёк)
  uint64_t lamports, fee;  // lamports — сумма перевода (у токена — в его единицах)
  const token_t *token;    // 0 — перевод SOL
  int create;              // создаётся счёт токена получателя (рента — с этого кошелька)
} sol_tx_t;

// owner/bump — для перевода токена (из аргументов команды); have_owner = 0 — их нет
static const char *sol_parse(const uint8_t *m, size_t n, const uint8_t own[32], const uint8_t owner[32],
                             int bump, int have_owner, sol_tx_t *tx) {
  rd_t r = {m, m + n, 0};
  int v0 = 0;
  if (n && (m[0] & 0x80)) {  // версионное сообщение: поддерживаем только v0
    if ((u8(&r) & 0x7f) != 0) return "unsupported message version";
    v0 = 1;
  }
  uint32_t req = u8(&r), ro_signed = u8(&r), ro_unsigned = u8(&r);
  uint32_t nkeys = compact(&r);
  if (r.err) return "bad message";
  if (req != 1 || ro_signed != 0) return "only one signature (this wallet) is supported";
  if (nkeys < 2 || nkeys > MAX_KEYS || ro_unsigned >= nkeys) return "bad account list";
  const uint8_t *keys = take(&r, nkeys * 32);
  take(&r, 32);  // recent blockhash
  if (r.err) return "bad message";
  if (memcmp(keys, own, 32)) return "fee payer is not this wallet";
  // доступ на запись: подписанный — индекс 0 (он и единственный), неподписанные — кроме последних ro
  uint32_t nix = compact(&r);
  if (r.err || nix < 1 || nix > MAX_IX) return "unsupported instructions";
  int transfers = 0, limits = 0, prices = 0, others = 0, tokens = 0, creates = 0;
  uint64_t cu_limit = 0, cu_price = 0;
  uint8_t t_src = 0, t_dst = 0, t_mint = 0, c_ata = 0, c_owner = 0, c_mint = 0;
  for (uint32_t k = 0; k < nix; k++) {
    uint32_t prog = u8(&r), na = compact(&r);
    const uint8_t *acc = take(&r, na);
    uint32_t dl = compact(&r);
    const uint8_t *d = take(&r, dl);
    if (r.err || prog >= nkeys) return "bad instruction";
    for (uint32_t a = 0; a < na; a++)
      if (acc[a] >= nkeys) return "bad instruction";
    const uint8_t *pid = keys + prog * 32;
    rd_t q = {d, d + dl, 0};
    if (!memcmp(pid, SYSTEM, 32)) {
      // transfer: u32 2, u64 lamports; счета [откуда (подпись, запись), куда (запись)]
      if (dl != 12 || le(&q, 4) != 2 || na != 2) return "only SOL transfers are supported";
      if (acc[0] != 0) return "transfer must be from this wallet";
      if (acc[1] == 0 || acc[1] >= nkeys - ro_unsigned) return "bad transfer destination";
      tx->lamports = le(&q, 8);
      memcpy(tx->to, keys + acc[1] * 32, 32);
      transfers++;
    } else if (!memcmp(pid, TOKEN, 32)) {
      // TransferChecked: u8 12, u64 сумма, u8 точность; счета [откуда, mint, куда, владелец-подписант]
      if (dl != 10 || le(&q, 1) != 12 || na != 4) return "only checked token transfers are supported";
      tx->lamports = le(&q, 8);
      uint32_t dec = (uint32_t)le(&q, 1);
      t_src = acc[0], t_mint = acc[1], t_dst = acc[2];
      if (acc[3] != 0) return "token transfer must be signed by this wallet";
      if (t_src == 0 || t_dst == 0 || t_src == t_dst) return "bad token accounts";
      if (t_src >= nkeys - ro_unsigned || t_dst >= nkeys - ro_unsigned) return "bad token accounts";
      if (!(tx->token = find_token(keys + t_mint * 32))) return "token not supported";
      if ((int)dec != tx->token->decimals) return "token decimals mismatch";
      tokens++;
    } else if (!memcmp(pid, ATA, 32)) {
      // CreateIdempotent ([1]) или Create ([]): [плательщик, ATA, владелец, mint, System, Token]
      if ((dl != 1 || d[0] != 1) && dl != 0) return "unsupported token account instruction";
      if (na != 6 || acc[0] != 0) return "bad token account creation";
      if (memcmp(keys + acc[4] * 32, SYSTEM, 32) || memcmp(keys + acc[5] * 32, TOKEN, 32)) return "bad token account creation";
      c_ata = acc[1], c_owner = acc[2], c_mint = acc[3];
      creates++;
    } else if (!memcmp(pid, COMPUTE_BUDGET, 32)) {
      uint32_t tag = (uint32_t)le(&q, 1);
      if (na != 0) return "bad compute budget";
      if (tag == 2 && dl == 5) {  // SetComputeUnitLimit(u32)
        cu_limit = le(&q, 4);
        limits++;
      } else if (tag == 3 && dl == 9) {  // SetComputeUnitPrice(u64, микро-lamports)
        cu_price = le(&q, 8);
        prices++;
      } else {
        return "unsupported compute budget instruction";
      }
    } else {
      return "unsupported program";
    }
    others += memcmp(pid, COMPUTE_BUDGET, 32) != 0;
  }
  if (v0) {  // таблицы адресов не поддерживаем: все счета — в самом сообщении
    if (compact(&r) != 0 || r.err) return "address lookup tables not supported";
  }
  if (r.err || r.p != r.end) return "bad message";
  if (transfers + tokens != 1 || limits > 1 || prices > 1 || creates > 1 || (creates && !tokens))
    return "exactly one transfer is supported";
  if (!tx->lamports) return "zero amount";
  if (tokens) {  // счёт получателя — ATA того владельца, которого увидит человек
    uint8_t want[32];
    if (!have_owner) return "recipient wallet required";
    ata_address(owner, keys + t_mint * 32, (uint8_t)bump, want);
    if (memcmp(want, keys + t_dst * 32, 32)) return "recipient token account mismatch";
    if (creates) {
      if (c_ata != t_dst || c_mint != t_mint || memcmp(keys + c_owner * 32, owner, 32))
        return "token account created for someone else";
      tx->create = 1;
    }
    memcpy(tx->to, owner, 32);
  }
  if (!limits) cu_limit = DEFAULT_CU * (uint64_t)others;
  if (cu_limit > 1400000) cu_limit = 1400000;
  // цена × лимит / 1e6, вверх; цена до 2^64 — считаем без переполнения
  uint64_t q1 = cu_price / 1000000, r1 = cu_price % 1000000;
  uint64_t prio = q1 * cu_limit + (r1 * cu_limit + 999999) / 1000000;
  tx->fee = BASE_FEE + prio;
  return 0;
}

// ---------------- команды ----------------

static void cmd_sign(uint32_t idx, const char *hex, size_t hl, const char *ow, size_t ol, const char *bw, size_t bl) {
  static uint8_t msg[1300];
  static sol_tx_t tx;
  uint32_t p[4];
  uint8_t own[32];
  path_of(idx, p);
  if (mw_ed25519_pubkey(p, 4, own) != 0) {
    mw_err("bad path");
    return;
  }
  int n = mw_unhex(hex, hl, msg, sizeof msg);
  if (n <= 0) {
    mw_err("bad hex");
    return;
  }
  memset(&tx, 0, sizeof tx);
  uint8_t owner[32];
  uint32_t bump = 0;
  int have_owner = ol > 0;
  if (have_owner && (!b58dec32(ow, ol, owner) || !mw_u32(bw, bl, &bump) || bump > 255)) {
    mw_err("bad recipient wallet");
    return;
  }
  const char *e = sol_parse(msg, (size_t)n, own, owner, (int)bump, have_owner, &tx);
  if (e) {
    mw_err(e);
    return;
  }
  char to[48], num[40], amt[56];
  b58(tx.to, 32, to);
  mw_fmt_u64(tx.lamports, tx.token ? tx.token->decimals : SOL_DECIMALS, num, sizeof num);
  mw_cat(amt, sizeof amt, num, " ", tx.token ? tx.token->symbol : "SOL");
  mw_amount("Amount", amt);
  mw_field("To", to);
  if (tx.create) {  // у получателя ещё нет счёта этого токена — его создаёт (и платит ренту) этот кошелёк
    mw_fmt_u64(TOKEN_ACCOUNT_RENT, SOL_DECIMALS, num, sizeof num);
    mw_cat(amt, sizeof amt, num, " SOL", 0);
    mw_amount("Account rent", amt);
  }
  if (tx.fee > HIGH_FEE) mw_warn("High fee!", "check it carefully");
  mw_fmt_u64(tx.fee, SOL_DECIMALS, num, sizeof num);
  mw_cat(amt, sizeof amt, num, " SOL", 0);
  mw_amount("Max fee", amt);
  if (!mw_net()) mw_field("Network", "Solana devnet");
  if (!mw_confirm(tx.token ? "Token transfer" : "Solana transfer")) {
    mw_err("rejected");
    return;
  }
  uint8_t sig[64];
  char out[129];
  if (mw_ed25519_sign(p, 4, msg, (size_t)n, sig) != 0) {
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
    const char *hx, *ow, *bw;
    size_t hl = mw_word(&p, end, &hx), ol = mw_word(&p, end, &ow), bl = mw_word(&p, end, &bw);
    cmd_sign(idx, hx, hl, ow, ol, bw, bl);
    return;
  }
  if (wl != 4 || (memcmp(w, "addr", 4) && memcmp(w, "show", 4))) {
    mw_err("unknown command");
    return;
  }
  uint32_t path[4];
  uint8_t pub[32];
  char a[48];
  path_of(idx, path);
  if (mw_ed25519_pubkey(path, 4, pub) != 0) {
    mw_err("bad path");
    return;
  }
  b58(pub, 32, a);
  if (w[0] == 'a') {
    mw_ok(a);
    return;
  }
  char title[24], num[11];
  mw_u32str(idx, num);
  mw_cat(title, sizeof title, "Solana #", num, 0);
  if (!mw_ui_address(title, mw_strlen(title), a, mw_strlen(a))) {
    mw_err("rejected");
    return;
  }
  mw_ok(a);
}
