#include "schnorr.h"

#include <string.h>

#include "bignum.h"
#include "ecdsa.h"
#include "memzero.h"
#include "secp256k1.h"

#define ORDER (&secp256k1.order)

void tagged_hash_init(SHA256_CTX *c, const char *tag) {
  uint8_t h[32];
  sha256_Raw((const uint8_t *)tag, strlen(tag), h);
  sha256_Init(c);
  sha256_Update(c, h, 32);
  sha256_Update(c, h, 32);
}

// P = d*G. Если у P нечётный Y, d заменяется на n - d (BIP340). x = P.x
static bool keypair(bignum256 *d, uint8_t x[32]) {
  curve_point P;
  if (bn_is_zero(d) || !bn_is_less(d, ORDER)) return false;
  if (scalar_multiply(&secp256k1, d, &P) != 0) return false;
  bn_mod(&P.y, &secp256k1.prime);
  bn_cnegate(bn_is_odd(&P.y), d, ORDER);
  bn_mod(d, ORDER);
  bn_write_be(&P.x, x);
  memzero(&P, sizeof P);
  return true;
}

bool bip340_pubkey(const uint8_t seckey[32], uint8_t x[32]) {
  bignum256 d;
  bn_read_be(seckey, &d);
  bool ok = keypair(&d, x);
  memzero(&d, sizeof d);
  return ok;
}

bool taproot_tweak_seckey(const uint8_t seckey[32], uint8_t out[32]) {
  bignum256 d, t;
  uint8_t px[32], h[32];
  SHA256_CTX c;

  bn_read_be(seckey, &d);
  bool ok = keypair(&d, px);
  if (ok) {
    tagged_hash_init(&c, "TapTweak");
    sha256_Update(&c, px, 32);
    sha256_Final(&c, h);
    bn_read_be(h, &t);
    ok = bn_is_less(&t, ORDER);
    bn_addmod(&d, &t, ORDER);
    bn_mod(&d, ORDER);
    ok = ok && !bn_is_zero(&d);
    bn_write_be(&d, out);
  }
  memzero(&d, sizeof d);
  memzero(&t, sizeof t);
  memzero(h, sizeof h);
  return ok;
}

bool bip340_sign(const uint8_t seckey[32], const uint8_t msg[32],
                 const uint8_t aux[32], uint8_t sig[64]) {
  bignum256 d, k, e;
  uint8_t px[32], t[32], h[32];
  curve_point R;
  SHA256_CTX c;
  bool ok = false;

  bn_read_be(seckey, &d);
  if (!keypair(&d, px)) goto out;

  // t = bytes(d) xor H_aux(a); k = H_nonce(t || P.x || m) mod n
  tagged_hash_init(&c, "BIP0340/aux");
  sha256_Update(&c, aux, 32);
  sha256_Final(&c, h);
  bn_write_be(&d, t);
  for (int i = 0; i < 32; i++) t[i] ^= h[i];
  tagged_hash_init(&c, "BIP0340/nonce");
  sha256_Update(&c, t, 32);
  sha256_Update(&c, px, 32);
  sha256_Update(&c, msg, 32);
  sha256_Final(&c, h);
  bn_read_be(h, &k);
  bn_mod(&k, ORDER);  // h < 2^256 < 2n, одного вычитания хватает
  if (bn_is_zero(&k) || scalar_multiply(&secp256k1, &k, &R) != 0) goto out;
  bn_mod(&R.y, &secp256k1.prime);
  bn_cnegate(bn_is_odd(&R.y), &k, ORDER);
  bn_mod(&k, ORDER);
  bn_mod(&R.x, &secp256k1.prime);
  bn_write_be(&R.x, sig);

  // e = H_challenge(R.x || P.x || m) mod n; s = k + e*d mod n
  tagged_hash_init(&c, "BIP0340/challenge");
  sha256_Update(&c, sig, 32);
  sha256_Update(&c, px, 32);
  sha256_Update(&c, msg, 32);
  sha256_Final(&c, h);
  bn_read_be(h, &e);
  bn_mod(&e, ORDER);
  bn_multiply(&d, &e, ORDER);
  bn_addmod(&e, &k, ORDER);
  bn_mod(&e, ORDER);
  bn_write_be(&e, sig + 32);
  ok = true;

out:
  memzero(&d, sizeof d);
  memzero(&k, sizeof k);
  memzero(&e, sizeof e);
  memzero(t, sizeof t);
  memzero(h, sizeof h);
  memzero(&R, sizeof R);
  return ok;
}
