// Тестовое приложение: большие данные.
// sum @   — SHA-256 загруженного блока двумя способами (прошивка и wasm), должны совпасть
// sum     — то же без данных: должно сказать, что их нет
// big <n> — ответ из n символов (mw_reply + mw_reply_more): хост забирает кусками
#include "mw.h"

static uint8_t buf[1000];

static void sum(void) {
  int n = mw_blob_size();
  if (n < 0) {
    mw_err("no blob");
    return;
  }
  uint32_t r[2] = {0, (uint32_t)n};
  uint8_t a[32], b[32];
  if (mw_blob_sha256(r, 1, 0, a)) {
    mw_err("blob_sha256 failed");
    return;
  }
  mw_sha256_t c;
  mw_sha256_init(&c);
  for (uint32_t o = 0; o < (uint32_t)n;) {
    int k = mw_blob_read(o, buf, sizeof buf);
    if (k <= 0) {
      mw_err("blob_read failed");
      return;
    }
    mw_sha256_update(&c, buf, (size_t)k);
    o += (uint32_t)k;
  }
  mw_sha256_final(&c, b);
  if (memcmp(a, b, 32)) {
    mw_err("hash mismatch");
    return;
  }
  char hex[65];
  mw_hex(a, 32, hex);
  mw_ok(hex);
}

static void big(const char *s, size_t len) {
  uint32_t n;
  if (!mw_u32(s, len, &n) || !n) {
    mw_err("usage: big <n>");
    return;
  }
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)('a' + i % 26);
  uint32_t k = n < sizeof buf ? n : sizeof buf;
  mw_reply((const char *)buf, k);
  for (n -= k; n; n -= k) {
    k = n < sizeof buf ? n : sizeof buf;
    mw_reply_more((const char *)buf, k);
  }
}

void mw_main(const char *cmd, size_t len) {
  if (len >= 3 && !memcmp(cmd, "sum", 3)) sum();
  else if (len > 4 && !memcmp(cmd, "big ", 4)) big(cmd + 4, len - 4);
  else mw_err("unknown command");
}
