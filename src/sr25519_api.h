// sr25519_api.h — то, что прошивка берёт из lib/sr25519-donna (schnorrkel, как в Substrate).
// Объявлено здесь, а не через их sr25519.h, чтобы их заголовки (свой sha2.h и т. п.)
// не попадали в пути поиска ядра.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// keypair: секрет 64 байта (ключ 32 + nonce 32) || публичный ключ 32
void sr25519_keypair_from_seed(uint8_t keypair[96], const uint8_t mini_secret[32]);
void sr25519_derive_keypair_hard(uint8_t out[96], const uint8_t in[96], const uint8_t chain_code[32]);
void sr25519_derive_keypair_soft(uint8_t out[96], const uint8_t in[96], const uint8_t chain_code[32]);
// Подпись с контекстом "substrate" (как sp_core::sr25519)
void sr25519_sign(uint8_t sig[64], const uint8_t pub[32], const uint8_t secret[64], const uint8_t *msg,
                  unsigned long len);
bool sr25519_verify(const uint8_t sig[64], const uint8_t *msg, unsigned long len, const uint8_t pub[32]);
