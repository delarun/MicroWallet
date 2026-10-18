// schnorr.h — подписи BIP340 и Taproot-твик BIP86 поверх примитивов trezor-crypto
// (в trezor-crypto BIP340 есть только через libsecp256k1-zkp, которой у нас нет)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "sha2.h"

// SHA256(SHA256(tag) || SHA256(tag) || ...): дальше sha256_Update/Final
void tagged_hash_init(SHA256_CTX *c, const char *tag);

// x-координата d*G (BIP340 x-only pubkey)
bool bip340_pubkey(const uint8_t seckey[32], uint8_t x[32]);

// BIP340: sig = R.x || s
bool bip340_sign(const uint8_t seckey[32], const uint8_t msg[32],
                 const uint8_t aux[32], uint8_t sig[64]);

// BIP86 (Taproot без дерева скриптов): d' = d(чётный Y) + H_TapTweak(P.x)
bool taproot_tweak_seckey(const uint8_t seckey[32], uint8_t out[32]);
