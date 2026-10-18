// sr25519_prefix.h — подключается принудительно (-include) к исходникам lib/sr25519-donna.
// Их глобальные имена получают префикс srd_, чтобы не столкнуться с trezor-crypto
// (sha2, ed25519-donna, memzero). Публичный API sr25519_* остаётся как есть.
// Список — все глобальные символы библиотеки (nm), кроме sr25519_*.
#pragma once
#include <stddef.h>
#define  srd_
#define bignum25519_ct_eq srd_bignum25519_ct_eq
#define curve25519_invsqrt srd_curve25519_invsqrt
#define curve25519_sqrt_ratio_i srd_curve25519_sqrt_ratio_i
#define derived_secret_key_simple srd_derived_secret_key_simple
#define derive_scalar_and_chaincode srd_derive_scalar_and_chaincode
#define divide_scalar_bytes_by_cofactor srd_divide_scalar_bytes_by_cofactor
#define EDWARDS_D srd_EDWARDS_D
#define EDWARDS_D_MINUS_ONE_SQUARED srd_EDWARDS_D_MINUS_ONE_SQUARED
#define elligator_ristretto_flavor srd_elligator_ristretto_flavor
#define expand_ed25519 srd_expand_ed25519
#define expand_uniform srd_expand_uniform
#define from_le_bytes srd_from_le_bytes
#define ge25519_move_conditional_pniels_array srd_ge25519_move_conditional_pniels_array
#define ge25519_scalarmult_tg srd_ge25519_scalarmult_tg
#define ge25519_set_neutral srd_ge25519_set_neutral
#define hard_derive_mini_secret_key srd_hard_derive_mini_secret_key
#define INVSQRT_A_MINUS_D srd_INVSQRT_A_MINUS_D
#define io_make_bytes srd_io_make_bytes
#define merlin_commit_witness_bytes srd_merlin_commit_witness_bytes
#define merlin_rng_commit_witness_bytes srd_merlin_rng_commit_witness_bytes
#define merlin_rng_finalize srd_merlin_rng_finalize
#define merlin_rng_init srd_merlin_rng_init
#define merlin_rng_random_bytes srd_merlin_rng_random_bytes
#define merlin_rng_wipe srd_merlin_rng_wipe
#define merlin_transcript_challenge_bytes srd_merlin_transcript_challenge_bytes
#define merlin_transcript_commit_bytes srd_merlin_transcript_commit_bytes
#define merlin_transcript_init srd_merlin_transcript_init
#define MINUS_ONE srd_MINUS_ONE
#define multiply_scalar_bytes_by_cofactor srd_multiply_scalar_bytes_by_cofactor
#define one srd_one
#define ONE_MINUS_EDWARDS_D_SQUARED srd_ONE_MINUS_EDWARDS_D_SQUARED
#define private_key_to_public_key srd_private_key_to_public_key
#define ristretto_ct_eq srd_ristretto_ct_eq
#define ristretto_decode srd_ristretto_decode
#define ristretto_encode srd_ristretto_encode
#define ristretto_from_uniform_bytes srd_ristretto_from_uniform_bytes
#define sha1_Data srd_sha1_Data
#define sha1_End srd_sha1_End
#define sha1_Final srd_sha1_Final
#define sha1_Init srd_sha1_Init
#define sha1_initial_hash_value srd_sha1_initial_hash_value
#define sha1_Raw srd_sha1_Raw
#define sha1_Transform srd_sha1_Transform
#define sha1_Update srd_sha1_Update
#define sha256_Data srd_sha256_Data
#define sha256_End srd_sha256_End
#define sha256_Final srd_sha256_Final
#define sha256_Init srd_sha256_Init
#define sha256_initial_hash_value srd_sha256_initial_hash_value
#define sha256_Raw srd_sha256_Raw
#define sha256_Transform srd_sha256_Transform
#define sha256_Update srd_sha256_Update
#define sha512_Data srd_sha512_Data
#define sha512_End srd_sha512_End
#define sha512_Final srd_sha512_Final
#define sha512_Init srd_sha512_Init
#define sha512_initial_hash_value srd_sha512_initial_hash_value
#define sha512_Raw srd_sha512_Raw
#define sha512_Transform srd_sha512_Transform
#define sha512_Update srd_sha512_Update
#define shorten_vrf srd_shorten_vrf
#define SQRT_AD_MINUS_ONE srd_SQRT_AD_MINUS_ONE
#define SQRT_M1 srd_SQRT_M1
#define uint8_32_ct_eq srd_uint8_32_ct_eq
#define vrf_sign srd_vrf_sign
#define vrf_verify srd_vrf_verify
#define zero srd_zero
// Свои memzero и источник случайности (src/sr25519_glue.c): аппаратный ГСЧ платы
#define memzero srd_memzero
#define explicit_bzero srd_memzero
#define randombytes_buf srd_random
void srd_memzero(void *const p, const size_t n);
