// mw.c — помощники SDK и входные точки модуля (mw_buf, mw_cmd)
#include "mw.h"

#define BUF_SIZE 4096
static char g_buf[BUF_SIZE];

__attribute__((export_name("mw_buf"))) char *mw_buf(void) { return g_buf; }

__attribute__((export_name("mw_cmd"))) void mw_cmd(int len) {
  if (len < 0 || len >= BUF_SIZE) len = 0;
  g_buf[len] = 0;
  mw_main(g_buf, (size_t)len);
}

// Без libc: компилятор может звать их сам (копирование структур)
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *dd = d;
  const uint8_t *ss = s;
  while (n--) *dd++ = *ss++;
  return d;
}

void *memset(void *d, int c, size_t n) {
  uint8_t *dd = d;
  while (n--) *dd++ = (uint8_t)c;
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

size_t mw_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

void mw_ok(const char *s) { mw_reply(s, mw_strlen(s)); }
void mw_err(const char *s) { mw_fail(s, mw_strlen(s)); }
void mw_field(const char *t, const char *v) { mw_ui_field(t, mw_strlen(t), v, mw_strlen(v)); }
int mw_confirm(const char *t) { return mw_ui_confirm(t, mw_strlen(t)); }

size_t mw_word(const char **p, const char *end, const char **w) {
  const char *s = *p;
  while (s < end && *s == ' ') s++;
  const char *e = s;
  while (e < end && *e != ' ') e++;
  *w = s;
  *p = e;
  return (size_t)(e - s);
}

int mw_u32(const char *s, size_t n, uint32_t *v) {
  uint64_t r = 0;
  if (!n || n > 10) return 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    r = r * 10 + (uint64_t)(s[i] - '0');
  }
  if (r > 0xFFFFFFFFu) return 0;
  *v = (uint32_t)r;
  return 1;
}

static int nib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int mw_unhex(const char *s, size_t n, uint8_t *out, size_t max) {
  if (n % 2 || n / 2 > max) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return (int)(n / 2);
}

void mw_hex(const uint8_t *b, size_t n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = H[b[i] >> 4];
    out[2 * i + 1] = H[b[i] & 15];
  }
  out[2 * n] = 0;
}

int mw_is_zero(const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (b[i]) return 0;
  return 1;
}

void mw_mul(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t *out) {
  size_t n = al + bl;
  memset(out, 0, n);
  for (size_t i = 0; i < al; i++) {
    uint32_t carry = 0;
    uint8_t x = a[al - 1 - i];
    for (size_t j = 0; j < bl; j++) {
      size_t k = n - 1 - i - j;
      uint32_t t = out[k] + (uint32_t)x * b[bl - 1 - j] + carry;
      out[k] = (uint8_t)t;
      carry = t >> 8;
    }
    for (size_t k = n - 1 - i - bl; carry; k--) {
      uint32_t t = out[k] + carry;
      out[k] = (uint8_t)t;
      carry = t >> 8;
      if (!k) break;
    }
  }
}

void mw_fmt_units(const uint8_t *be, size_t n, int decimals, char *out, size_t outlen) {
  uint8_t v[64];
  char dig[160];
  size_t nd = 0;
  if (n > sizeof v) n = sizeof v;
  memcpy(v, be, n);
  // десятичные цифры делением на 10, младшие первыми
  while (!mw_is_zero(v, n) && nd < sizeof dig) {
    uint32_t rem = 0;
    for (size_t i = 0; i < n; i++) {
      uint32_t cur = rem << 8 | v[i];
      v[i] = (uint8_t)(cur / 10);
      rem = cur % 10;
    }
    dig[nd++] = (char)('0' + rem);
  }
  size_t o = 0;
  int intlen = (int)nd - decimals;
  if (intlen <= 0) {
    if (o < outlen - 1) out[o++] = '0';
  } else {
    for (int i = 0; i < intlen && o < outlen - 1; i++) out[o++] = dig[nd - 1 - i];
  }
  // дробная часть без хвостовых нулей
  int last = -1;
  for (int i = 0; i < decimals; i++) {
    int idx = decimals - 1 - i;  // цифра i после точки
    char c = idx < (int)nd ? dig[idx] : '0';
    if (c != '0') last = i;
  }
  if (last >= 0 && o < outlen - 1) {
    out[o++] = '.';
    for (int i = 0; i <= last && o < outlen - 1; i++) {
      int idx = decimals - 1 - i;
      out[o++] = idx < (int)nd ? dig[idx] : '0';
    }
  }
  out[o] = 0;
}

size_t mw_cat(char *out, size_t outlen, const char *a, const char *b, const char *c) {
  size_t o = 0;
  const char *parts[3] = {a, b, c};
  for (int p = 0; p < 3; p++)
    for (const char *s = parts[p]; s && *s && o < outlen - 1; s++) out[o++] = *s;
  out[o] = 0;
  return o;
}

void mw_amount(const char *t, const char *v) { mw_ui_amount(t, mw_strlen(t), v, mw_strlen(v)); }
void mw_warn(const char *t, const char *v) { mw_ui_warn(t, mw_strlen(t), v, mw_strlen(v)); }

void mw_u32str(uint32_t v, char *out) {
  char d[11];
  int n = 0;
  do d[n++] = (char)('0' + v % 10), v /= 10;
  while (v);
  for (int i = 0; i < n; i++) out[i] = d[n - 1 - i];
  out[n] = 0;
}

void mw_fmt_u64(uint64_t v, int decimals, char *out, size_t outlen) {
  uint8_t be[8];
  for (int i = 0; i < 8; i++) be[7 - i] = (uint8_t)(v >> (8 * i));
  mw_fmt_units(be, 8, decimals, out, outlen);
}

void mw_compress(const uint8_t pub65[65], uint8_t out[33]) {
  out[0] = (uint8_t)(0x02 | (pub65[64] & 1));
  memcpy(out + 1, pub65 + 1, 32);
}

// ---------- SHA-256 (FIPS 180-4) ----------

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

void mw_sha256_init(mw_sha256_t *c) {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, H0, sizeof H0);
  c->len = 0;
  c->n = 0;
}

void mw_sha256_update(mw_sha256_t *c, const void *data, size_t len) {
  const uint8_t *p = data;
  c->len += len;
  while (len) {
    size_t take = 64 - c->n < len ? 64 - c->n : len;
    memcpy(c->buf + c->n, p, take);
    c->n += (uint32_t)take;
    p += take;
    len -= take;
    if (c->n == 64) {
      sha_block(c->h, c->buf);
      c->n = 0;
    }
  }
}

void mw_sha256_final(mw_sha256_t *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80, z = 0, l[8];
  mw_sha256_update(c, &pad, 1);
  while (c->n != 56) mw_sha256_update(c, &z, 1);
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  mw_sha256_update(c, l, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}

// ---------- bech32 / bech32m (BIP173, BIP350) ----------

static uint32_t pm_step(uint32_t pre) {
  uint32_t b = pre >> 25;
  return ((pre & 0x1FFFFFF) << 5) ^ ((0u - (b & 1)) & 0x3b6a57b2u) ^ ((0u - ((b >> 1) & 1)) & 0x26508e6du) ^
         ((0u - ((b >> 2) & 1)) & 0x1ea119fau) ^ ((0u - ((b >> 3) & 1)) & 0x3d4233ddu) ^
         ((0u - ((b >> 4) & 1)) & 0x2a1462b3u);
}

int mw_segwit_addr(char *out, size_t outlen, const char *hrp, int ver, const uint8_t *prog, size_t len) {
  static const char CS[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  uint8_t d[66];
  size_t dl = 0, hl = mw_strlen(hrp);
  uint32_t acc = 0, bits = 0;
  if (ver < 0 || ver > 16 || len < 2 || len > 40) return 0;
  d[dl++] = (uint8_t)ver;
  for (size_t i = 0; i < len; i++) {
    acc = acc << 8 | prog[i];
    bits += 8;
    while (bits >= 5) d[dl++] = (acc >> (bits -= 5)) & 31;
  }
  if (bits) d[dl++] = (acc << (5 - bits)) & 31;
  if (hl + 1 + dl + 6 + 1 > outlen) return 0;
  uint32_t chk = 1;
  for (size_t i = 0; i < hl; i++) chk = pm_step(chk) ^ ((uint8_t)hrp[i] >> 5);
  chk = pm_step(chk);
  size_t o = 0;
  for (size_t i = 0; i < hl; i++) {
    chk = pm_step(chk) ^ (hrp[i] & 31);
    out[o++] = hrp[i];
  }
  out[o++] = '1';
  for (size_t i = 0; i < dl; i++) {
    chk = pm_step(chk) ^ d[i];
    out[o++] = CS[d[i]];
  }
  for (int i = 0; i < 6; i++) chk = pm_step(chk);
  chk ^= ver ? 0x2bc830a3u : 1u;
  for (int i = 0; i < 6; i++) out[o++] = CS[(chk >> ((5 - i) * 5)) & 31];
  out[o] = 0;
  return 1;
}

// ---------- base58check ----------

int mw_base58check(const uint8_t *data, size_t len, char *out, size_t outlen) {
  static const char A[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t b[64], h[32], dig[96];
  size_t n = len + 4, nd = 0, zeros = 0;
  if (n > sizeof b) return 0;
  memcpy(b, data, len);
  mw_sha256(data, len, h);
  mw_sha256(h, 32, h);
  memcpy(b + len, h, 4);
  while (zeros < n && !b[zeros]) zeros++;
  // деление на 58 большого числа b
  for (size_t start = zeros; start < n;) {
    uint32_t rem = 0;
    for (size_t i = start; i < n; i++) {
      uint32_t cur = rem << 8 | b[i];
      b[i] = (uint8_t)(cur / 58);
      rem = cur % 58;
    }
    dig[nd++] = (uint8_t)rem;
    while (start < n && !b[start]) start++;
  }
  if (zeros + nd + 1 > outlen) return 0;
  size_t o = 0;
  for (size_t i = 0; i < zeros; i++) out[o++] = '1';
  for (size_t i = 0; i < nd; i++) out[o++] = A[dig[nd - 1 - i]];
  out[o] = 0;
  return 1;
}
