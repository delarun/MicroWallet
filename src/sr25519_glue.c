// sr25519_glue.c — то, что lib/sr25519-donna берёт снаружи (имена — из sr25519_prefix.h)
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

void srd_memzero(void *const p, const size_t n) {
  volatile uint8_t *v = p;
  for (size_t i = 0; i < n; i++) v[i] = 0;
}

// Случайность для подписи schnorrkel (вместе с секретом в транскрипте Merlin)
void srd_random(void *const buf, const size_t size) { hal_random(buf, size); }
