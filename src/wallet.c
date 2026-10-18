// wallet.c — ключи BIP39/BIP32 поверх trezor-crypto. Адреса монет — в приложениях.
#include "wallet.h"

#include <string.h>

#include "base58.h"
#include "bignum.h"
#include "bip39.h"
#include "ecdsa.h"
#include "ed25519-donna/ed25519.h"
#include "pbkdf2.h"
#include "hmac.h"
#include "memzero.h"
#include "ripemd160.h"
#include "schnorr.h"
#include "secp256k1.h"
#include "sr25519_api.h"
#include "sha2.h"

#define HARDENED 0x80000000u

typedef struct {
  uint8_t depth;
  uint32_t parent_fp;
  uint32_t child_num;
  uint8_t chain_code[32];
  uint8_t priv[32];
  uint8_t pub[33];
} hdnode_t;

static hdnode_t g_master;  // m; всё остальное выводится по запросу приложений
static bool g_unlocked;

// ed25519 (SLIP-10): ключ || chain code корня — HMAC-SHA512("ed25519 seed", seed)
static uint8_t g_ed_master[64];
// sr25519 (Substrate): мини-секрет — PBKDF2 от энтропии фразы, считается при первом запросе
static uint8_t g_entropy[32], g_sr_mini[32];
static size_t g_entropy_len;
static bool g_sr_ready;

// Кэш недавних узлов: приложения чаще всего просят соседние адреса одной ветки
// (m/84'/0'/0'/0/i), и родитель такого пути выводится один раз
#define CACHE 4
static struct {
  int depth;
  uint32_t path[WALLET_PATH_MAX];
  hdnode_t node;
} g_cache[CACHE];
static int g_cache_next;

void hash160(const uint8_t *in, size_t len, uint8_t out[20]) {
  uint8_t h[32];
  sha256_Raw(in, len, h);
  ripemd160(h, 32, out);
  memzero(h, sizeof h);
}

static uint32_t fingerprint(const uint8_t pub[33]) {
  uint8_t h[20];
  hash160(pub, 33, h);
  return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
         ((uint32_t)h[2] << 8) | h[3];
}

static bool node_from_seed(const uint8_t seed[64], hdnode_t *n) {
  uint8_t I[64];
  bignum256 k;
  hmac_sha512((const uint8_t *)"Bitcoin seed", 12, seed, 64, I);
  bn_read_be(I, &k);
  bool ok = !bn_is_zero(&k) && bn_is_less(&k, &secp256k1.order);
  memset(n, 0, sizeof *n);
  memcpy(n->priv, I, 32);
  memcpy(n->chain_code, I + 32, 32);
  if (ok) ok = ecdsa_get_public_key33(&secp256k1, n->priv, n->pub) == 0;
  memzero(I, sizeof I);
  memzero(&k, sizeof k);
  return ok;
}

// CKDpriv из BIP32
static bool ckd_priv(const hdnode_t *p, uint32_t i, hdnode_t *c) {
  uint8_t data[37], I[64];
  bignum256 il, k;
  hdnode_t t;

  if (i & HARDENED) {
    data[0] = 0;
    memcpy(data + 1, p->priv, 32);
  } else {
    memcpy(data, p->pub, 33);
  }
  data[33] = i >> 24; data[34] = i >> 16; data[35] = i >> 8; data[36] = i;
  hmac_sha512(p->chain_code, 32, data, sizeof data, I);

  bn_read_be(I, &il);
  bool ok = bn_is_less(&il, &secp256k1.order);
  bn_read_be(p->priv, &k);
  bn_add(&k, &il);  // k, il < n  =>  k + il < 2n
  bn_mod(&k, &secp256k1.order);
  ok = ok && !bn_is_zero(&k);

  t.depth = p->depth + 1;
  t.parent_fp = fingerprint(p->pub);
  t.child_num = i;
  memcpy(t.chain_code, I + 32, 32);
  bn_write_be(&k, t.priv);
  if (ok) ok = ecdsa_get_public_key33(&secp256k1, t.priv, t.pub) == 0;
  if (ok) *c = t;

  memzero(data, sizeof data);
  memzero(I, sizeof I);
  memzero(&il, sizeof il);
  memzero(&k, sizeof k);
  memzero(&t, sizeof t);
  return ok;
}

bool wallet_unlock(const uint8_t *entropy, size_t len,
                   void (*progress)(uint32_t, uint32_t)) {
  static uint8_t seed[64];
  wallet_lock();
  const char *mnemonic = mnemonic_from_data(entropy, len);
  if (!mnemonic) return false;
  mnemonic_to_seed(mnemonic, "", seed, progress);  // PBKDF2, 2048 итераций
  mnemonic_clear();
  bool ok = node_from_seed(seed, &g_master);
  hmac_sha512((const uint8_t *)"ed25519 seed", 12, seed, 64, g_ed_master);
  memcpy(g_entropy, entropy, len);
  g_entropy_len = len;
  memzero(seed, sizeof seed);
  if (!ok) wallet_lock();
  g_unlocked = ok;
  return ok;
}

void wallet_lock(void) {
  memzero(&g_master, sizeof g_master);
  memzero(g_ed_master, sizeof g_ed_master);
  memzero(g_entropy, sizeof g_entropy);
  memzero(g_sr_mini, sizeof g_sr_mini);
  g_entropy_len = 0;
  g_sr_ready = false;
  memzero(g_cache, sizeof g_cache);
  g_unlocked = false;
}

bool wallet_is_unlocked(void) { return g_unlocked; }

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// Узел по пути: от ближайшего закэшированного предка (или от корня)
static bool path_node(const uint32_t *path, int depth, hdnode_t *out) {
  if (!g_unlocked || depth < 0 || depth > WALLET_PATH_MAX) return false;
  hdnode_t a = g_master, b;
  int from = 0;
  for (int i = 0; i < CACHE; i++) {
    int d = g_cache[i].depth;
    if (d > from && d <= depth && !memcmp(g_cache[i].path, path, d * sizeof *path)) {
      a = g_cache[i].node;
      from = d;
    }
  }
  bool ok = true;
  for (int i = from; i < depth && ok; i++) {
    ok = ckd_priv(&a, path[i], &b);
    a = b;
    // Родителя листа запоминаем: следующий адрес той же ветки — одна ступень
    if (ok && i == depth - 2 && depth >= 2) {
      int c = g_cache_next++ % CACHE;
      g_cache[c].depth = depth - 1;
      memcpy(g_cache[c].path, path, (depth - 1) * sizeof *path);
      g_cache[c].node = a;
    }
  }
  if (ok) *out = a;
  memzero(&a, sizeof a);
  memzero(&b, sizeof b);
  return ok;
}

bool wallet_path_key(const uint32_t *path, int depth, uint8_t priv[32], uint8_t pub65[65]) {
  hdnode_t n;
  if (depth < 1 || !path_node(path, depth, &n)) return false;
  if (pub65) ecdsa_get_public_key65(&secp256k1, n.priv, pub65);
  if (priv) memcpy(priv, n.priv, 32);
  memzero(&n, sizeof n);
  return true;
}

bool wallet_path_sign(const uint32_t *path, int depth, const uint8_t digest[32], uint8_t sig[64],
                      uint8_t *recid) {
  uint8_t priv[32];
  if (!wallet_path_key(path, depth, priv, NULL)) return false;
  int r = ecdsa_sign_digest(&secp256k1, priv, digest, sig, recid, NULL);
  memzero(priv, sizeof priv);
  return r == 0;
}

bool wallet_path_xpub(const uint32_t *path, int depth, uint32_t version, char *out, size_t outlen) {
  hdnode_t n;
  if (!path_node(path, depth, &n)) return false;
  uint8_t b[78];
  put_be32(b, version);
  b[4] = n.depth;
  put_be32(b + 5, n.parent_fp);
  put_be32(b + 9, n.child_num);
  memcpy(b + 13, n.chain_code, 32);
  memcpy(b + 45, n.pub, 33);
  memzero(&n, sizeof n);
  return base58_encode_check(b, sizeof b, HASHER_SHA2D, out, outlen) > 0;
}

// BIP86 key-path: ключ выхода Q = P + H_TapTweak(P)·G (без дерева скриптов)
static bool taproot_seckey(const uint32_t *path, int depth, uint8_t tweaked[32]) {
  uint8_t priv[32];
  bool ok = wallet_path_key(path, depth, priv, NULL) && taproot_tweak_seckey(priv, tweaked);
  memzero(priv, sizeof priv);
  return ok;
}

bool wallet_path_taproot(const uint32_t *path, int depth, uint8_t x[32]) {
  uint8_t tw[32];
  bool ok = taproot_seckey(path, depth, tw) && bip340_pubkey(tw, x);
  memzero(tw, sizeof tw);
  return ok;
}

bool wallet_path_taproot_sign(const uint32_t *path, int depth, const uint8_t msg[32],
                              const uint8_t aux[32], uint8_t sig[64]) {
  uint8_t tw[32];
  bool ok = taproot_seckey(path, depth, tw) && bip340_sign(tw, msg, aux, sig);
  memzero(tw, sizeof tw);
  return ok;
}

// ---------- ed25519 (SLIP-10): Solana, TON, Stellar, Aptos … ----------
// Только hardened-уровни: у ed25519 нет публичной деривации.

static bool ed_key(const uint32_t *path, int depth, uint8_t key[32]) {
  if (!g_unlocked || depth < 1 || depth > WALLET_PATH_MAX) return false;
  uint8_t I[64], next[64], data[37];
  memcpy(I, g_ed_master, 64);
  bool ok = true;
  for (int i = 0; i < depth && ok; i++) {
    if (!(path[i] & HARDENED)) {
      ok = false;
      break;
    }
    data[0] = 0;
    memcpy(data + 1, I, 32);
    put_be32(data + 33, path[i]);
    hmac_sha512(I + 32, 32, data, sizeof data, next);
    memcpy(I, next, 64);
  }
  if (ok) memcpy(key, I, 32);
  memzero(I, sizeof I);
  memzero(next, sizeof next);
  memzero(data, sizeof data);
  return ok;
}

bool wallet_ed25519_pubkey(const uint32_t *path, int depth, uint8_t pub[32]) {
  uint8_t k[32];
  if (!ed_key(path, depth, k)) return false;
  ed25519_publickey(k, pub);
  memzero(k, sizeof k);
  return true;
}

bool wallet_ed25519_sign(const uint32_t *path, int depth, const uint8_t *msg, size_t len, uint8_t sig[64]) {
  uint8_t k[32];
  if (!ed_key(path, depth, k)) return false;
  ed25519_sign(msg, len, k, sig);
  memzero(k, sizeof k);
  return true;
}

// ---------- sr25519 (schnorrkel, Substrate): Polkadot, Kusama … ----------
// junctions: n × 33 байт — [1 = hard (//), 0 = soft (/)] + chain code 32 байта

static bool sr_keypair(const uint8_t *junctions, int n, uint8_t kp[96]) {
  if (!g_unlocked || n < 0 || n > WALLET_PATH_MAX) return false;
  if (!g_sr_ready) {  // substrate-bip39: PBKDF2-HMAC-SHA512(энтропия, "mnemonic"), 2048 раундов
    uint8_t seed[64];
    pbkdf2_hmac_sha512(g_entropy, g_entropy_len, (const uint8_t *)"mnemonic", 8, 2048, seed, 64);
    memcpy(g_sr_mini, seed, 32);
    memzero(seed, sizeof seed);
    g_sr_ready = true;
  }
  uint8_t t[96];
  sr25519_keypair_from_seed(kp, g_sr_mini);
  for (int i = 0; i < n; i++) {
    const uint8_t *j = junctions + 33 * i;
    if (j[0] > 1) return false;
    if (j[0]) sr25519_derive_keypair_hard(t, kp, j + 1);
    else sr25519_derive_keypair_soft(t, kp, j + 1);
    memcpy(kp, t, 96);
  }
  memzero(t, sizeof t);
  return true;
}

bool wallet_sr25519_pubkey(const uint8_t *junctions, int n, uint8_t pub[32]) {
  uint8_t kp[96];
  bool ok = sr_keypair(junctions, n, kp);
  if (ok) memcpy(pub, kp + 64, 32);
  memzero(kp, sizeof kp);
  return ok;
}

bool wallet_sr25519_sign(const uint8_t *junctions, int n, const uint8_t *msg, size_t len, uint8_t sig[64]) {
  uint8_t kp[96];
  bool ok = sr_keypair(junctions, n, kp);
  if (ok) sr25519_sign(sig, kp + 64, kp, msg, len);
  memzero(kp, sizeof kp);
  return ok;
}
