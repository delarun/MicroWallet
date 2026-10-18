// Проверка подписи sr25519 для тестов: sr25519_verify <sig hex> <msg hex> <pubkey hex>
// Собирается с объектами lib/sr25519-donna из tools/sim/build.sh (build/sim-sr25519).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool sr25519_verify(const uint8_t sig[64], const uint8_t *msg, unsigned long len, const uint8_t pub[32]);

void srd_memzero(void *const p, const size_t n) { memset(p, 0, n); }
void srd_random(void *const buf, const size_t n) { memset(buf, 0, n); }  // проверке не нужна

static size_t unhex(const char *s, uint8_t *out, size_t max) {
  size_t n = strlen(s) / 2;
  for (size_t i = 0; i < n && i < max; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
  return n;
}

int main(int argc, char **argv) {
  uint8_t sig[64], pub[32], msg[1024];
  if (argc != 4) return 2;
  unhex(argv[1], sig, 64);
  size_t ml = unhex(argv[2], msg, sizeof msg);
  unhex(argv[3], pub, 32);
  bool ok = sr25519_verify(sig, msg, ml, pub);
  puts(ok ? "ok" : "bad");
  return ok ? 0 : 1;
}
