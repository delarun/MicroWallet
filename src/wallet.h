// wallet.h — ключи (BIP39 / BIP32) без привязки к монетам и железу.
// Адреса и разбор транзакций — в приложениях (apps/), они получают ключи через src/apps.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WALLET_PATH_MAX 8  // глубина пути BIP32

// Разблокировать кошелёк: энтропия -> мнемоника -> seed -> корневой узел
bool wallet_unlock(const uint8_t *entropy, size_t len,
                   void (*progress)(uint32_t cur, uint32_t total));
void wallet_lock(void);
bool wallet_is_unlocked(void);
// Отпечаток мастер-ключа (BIP32: первые 4 байта hash160 ключа m) — для PSBT и дескрипторов
bool wallet_master_fingerprint(uint32_t *fp);

// Ключ по пути: path[i] | 0x80000000 — hardened. pub65 — несжатый (04 || x || y).
// Любой из выходов может быть NULL.
bool wallet_path_key(const uint32_t *path, int depth, uint8_t priv[32], uint8_t pub65[65]);
// ECDSA secp256k1 (RFC 6979, low-s): sig = r || s, recid = 0/1
bool wallet_path_sign(const uint32_t *path, int depth, const uint8_t digest[32], uint8_t sig[64],
                      uint8_t *recid);
// Совпадёт ли вывод пути у btcd/btcutil DeriveNonStandard (Pearl Wallet, oyster): он хэширует
// приватный ключ выведенного (не корневого) родителя без ведущих нулевых байтов, и hardened-
// потомки такого узла у него другие (btcd #172). *same = false — адреса Pearl Wallet с той же
// фразой будут не такими, как у нас. Наружу — один бит, ключи не раскрываются.
bool wallet_path_btcd_same(const uint32_t *path, int depth, bool *same);
// Расширенный публичный ключ узла (version: xpub/zpub/vpub …) в base58check
bool wallet_path_xpub(const uint32_t *path, int depth, uint32_t version, char *out, size_t outlen);
void hash160(const uint8_t *in, size_t len, uint8_t out[20]);  // RIPEMD160(SHA256(x))

// Taproot BIP86 (key-path): x-координата ключа выхода и BIP340-подпись им
bool wallet_path_taproot(const uint32_t *path, int depth, uint8_t x[32]);
bool wallet_path_taproot_sign(const uint32_t *path, int depth, const uint8_t msg[32],
                              const uint8_t aux[32], uint8_t sig[64]);

// ed25519 по SLIP-10 (все уровни hardened): Solana, TON, Stellar, Aptos, Sui …
bool wallet_ed25519_pubkey(const uint32_t *path, int depth, uint8_t pub[32]);
bool wallet_ed25519_sign(const uint32_t *path, int depth, const uint8_t *msg, size_t len, uint8_t sig[64]);

// sr25519 как в Substrate (Polkadot, Kusama): корень — от энтропии фразы (substrate-bip39),
// дальше «//hard» и «/soft». junctions: n × 33 байт — флаг hard (1/0) + chain code 32 байта.
bool wallet_sr25519_pubkey(const uint8_t *junctions, int n, uint8_t pub[32]);
bool wallet_sr25519_sign(const uint8_t *junctions, int n, const uint8_t *msg, size_t len, uint8_t sig[64]);
