// mw.h — SDK приложений MicroWallet (WebAssembly).
//
// Приложение — папка с исходником .c и файлом manifest; в .c — функция mw_main(cmd, len),
// которая отвечает ровно один раз: mw_ok("текст") или mw_err("причина").
// Сборка: sdk/build.sh apps/eth/eth.c build/eth.wasm
//
// Прошивка проверяет каждое обращение:
//   - ключи (mw_pubkey, mw_sign) — только по путям из манифеста (path=...);
//   - mw_sign — только если человек подтвердил экран mw_ui_confirm в этом же вызове.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MW_IMPORT(name) __attribute__((import_module("mw"), import_name(name)))

// ---- ответ ----
MW_IMPORT("reply") void mw_reply(const char *s, size_t len);  // ok <текст>
MW_IMPORT("fail") void mw_fail(const char *s, size_t len);    // err <текст>
// Дописать к ответу (после mw_reply): длинный ответ хост заберёт кусками — до 256 КБ
MW_IMPORT("reply_more") void mw_reply_more(const char *s, size_t len);

// ---- большой вход: команда кончается на « @», данные хост загрузил заранее (app put) ----
MW_IMPORT("blob_size") int mw_blob_size(void);  // размер, -1 — у вызова нет данных
MW_IMPORT("blob_read") int mw_blob_read(uint32_t off, void *buf, uint32_t len);  // прочитано, -1 — ошибка
// SHA-256 (dbl=1 — двойной) склейки n кусков блока: ranges = {смещение, длина} × n, n ≤ 64
MW_IMPORT("blob_sha256") int mw_blob_sha256(const uint32_t *ranges, int n, int dbl, uint8_t out[32]);

// ---- сеть сборки: 0 — testnet, 1 — mainnet ----
MW_IMPORT("net") int mw_net(void);

// ---- ключи secp256k1 ----
// path: элементы BIP32, H(x) — hardened. 0 — успех, <0 — запрещено/ошибка.
// pubkey: 65 байт несжатого ключа (04 || x || y).
MW_IMPORT("pubkey") int mw_pubkey(const uint32_t *path, int depth, uint8_t out[65]);
// sign: ECDSA (RFC 6979, low-s) хеша; out = r(32) || s(32) || recid(1)
MW_IMPORT("sign") int mw_sign(const uint32_t *path, int depth, const uint8_t hash[32], uint8_t out[65]);

// xpub узла (version — 4 байта префикса: xpub/zpub/vpub …): длина строки или <0
MW_IMPORT("xpub") int mw_xpub(const uint32_t *path, int depth, uint32_t version, char *out, size_t outlen);
// Taproot BIP86 key-path: x-координата ключа выхода и BIP340-подпись (aux — от ГСЧ прошивки)
MW_IMPORT("taproot_key") int mw_taproot_key(const uint32_t *path, int depth, uint8_t x[32]);
MW_IMPORT("taproot_sign") int mw_taproot_sign(const uint32_t *path, int depth, const uint8_t msg[32], uint8_t sig[64]);

// ed25519 по SLIP-10 (все уровни пути hardened): Solana, TON, Stellar …
MW_IMPORT("ed25519_pubkey") int mw_ed25519_pubkey(const uint32_t *path, int depth, uint8_t pub[32]);
MW_IMPORT("ed25519_sign") int mw_ed25519_sign(const uint32_t *path, int depth, const void *msg, size_t len, uint8_t sig[64]);
// sr25519 как в Substrate (нужно sr25519=yes в манифесте). junctions: n × 33 байт —
// [1 — hard «//», 0 — soft «/»] + chain code 32 байта (SCALE-кодированное имя, дополненное нулями)
MW_IMPORT("sr25519_pubkey") int mw_sr25519_pubkey(const uint8_t *junctions, int n, uint8_t pub[32]);
MW_IMPORT("sr25519_sign") int mw_sr25519_sign(const uint8_t *junctions, int n, const void *msg, size_t len, uint8_t sig[64]);

// ---- хеши (на прошивке быстрее, чем в wasm) ----
MW_IMPORT("blake2b") int mw_blake2b(const void *data, size_t len, size_t outlen, uint8_t *out);  // 1..64 байт
MW_IMPORT("keccak256") void mw_keccak256(const void *data, size_t len, uint8_t out[32]);
MW_IMPORT("sha256") void mw_sha256(const void *data, size_t len, uint8_t out[32]);
MW_IMPORT("hash160") int mw_hash160(const void *data, size_t len, uint8_t out[20]);

// ---- экраны ----
// Шаги экрана проверки (до 40), затем mw_ui_confirm: «Review <что>», шаги, Accept/Reject.
MW_IMPORT("ui_field") void mw_ui_field(const char *title, size_t tl, const char *text, size_t vl);   // текст
MW_IMPORT("ui_amount") void mw_ui_amount(const char *title, size_t tl, const char *text, size_t vl); // сумма крупно
MW_IMPORT("ui_warn") void mw_ui_warn(const char *title, size_t tl, const char *text, size_t vl);    // предупреждение
MW_IMPORT("ui_confirm") int mw_ui_confirm(const char *title, size_t tl);  // 1 — Accept
// Показать адрес для сверки: 1 — Approve
MW_IMPORT("ui_address") int mw_ui_address(const char *title, size_t tl, const char *addr, size_t al);

#define H(x) ((uint32_t)(x) | 0x80000000u)

// ---- приложение реализует ----
void mw_main(const char *cmd, size_t len);

// ---- помощники (sdk/mw.c) ----
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t mw_strlen(const char *s);
void mw_ok(const char *s);
void mw_err(const char *s);
void mw_field(const char *title, const char *text);
void mw_amount(const char *title, const char *text);
void mw_warn(const char *title, const char *text);
int mw_confirm(const char *title);

// Следующее слово команды: *p сдвигается; 0 — слов больше нет
size_t mw_word(const char **p, const char *end, const char **w);
int mw_u32(const char *s, size_t n, uint32_t *v);           // десятичное
int mw_unhex(const char *s, size_t n, uint8_t *out, size_t max);  // -> число байт или -1
void mw_hex(const uint8_t *b, size_t n, char *out);          // + завершающий 0

// Беззнаковые big-endian числа
int mw_is_zero(const uint8_t *b, size_t n);
// out (al + bl байт) = a * b
void mw_mul(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t *out);
// Сумма в единицах -> "12.5" (decimals знаков после точки, хвостовые нули убраны)
void mw_fmt_units(const uint8_t *be, size_t n, int decimals, char *out, size_t outlen);
// Склеить строки в out (через memcpy), вернуть длину
size_t mw_cat(char *out, size_t outlen, const char *a, const char *b, const char *c);
// Целое -> "123"; сумма u64 в единицах -> "0.0005"
void mw_u32str(uint32_t v, char *out);
void mw_fmt_u64(uint64_t v, int decimals, char *out, size_t outlen);

// ---- SHA-256 по частям (в wasm; для BIP143/BIP341) ----
typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  uint32_t n;
} mw_sha256_t;
void mw_sha256_init(mw_sha256_t *c);
void mw_sha256_update(mw_sha256_t *c, const void *data, size_t len);
void mw_sha256_final(mw_sha256_t *c, uint8_t out[32]);

// ---- адреса ----
// segwit: v0 — bech32, v1+ — bech32m. 1 — успех
int mw_segwit_addr(char *out, size_t outlen, const char *hrp, int ver, const uint8_t *prog, size_t len);
// base58 с контрольной суммой (двойной SHA-256). 1 — успех
int mw_base58check(const uint8_t *data, size_t len, char *out, size_t outlen);
// Сжатый ключ из несжатого (04 || x || y)
void mw_compress(const uint8_t pub65[65], uint8_t out[33]);
