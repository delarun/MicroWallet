#include "channel.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "chacha20poly1305/rfc7539.h"
#include "ed25519-donna/ed25519.h"
#include "hal.h"
#include "hmac.h"
#include "memzero.h"
#include "sha2.h"

enum { T_HELLO = 0x01, T_HELLO_R = 0x02, T_ERROR = 0x03, T_DATA = 0x10 };
#define TAG 16
#define HDR 9  // тип + счётчик

static struct {
  bool active;
  uint8_t k_rx[32], k_tx[32];  // ПК->устройство, устройство->ПК
  uint64_t rx_next, tx_next;
} S;

static chan_send_fn g_send;

void chan_reset(void) { memzero(&S, sizeof S); }

bool chan_active(void) { return S.active; }

static void send_error(const char *msg) {
  uint8_t f[64];
  size_t n = strlen(msg);
  if (n > sizeof f - 1) n = sizeof f - 1;
  f[0] = T_ERROR;
  memcpy(f + 1, msg, n);
  g_send(f, n + 1);
}

static void put_be64(uint8_t *p, uint64_t v) {
  for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v;
}

static uint64_t get_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static void nonce_of(uint64_t ctr, uint8_t nonce[12]) {
  memset(nonce, 0, 4);
  put_be64(nonce + 4, ctr);
}

// HKDF-Expand с одним блоком: HMAC(prk, label || 0x01)
static void expand(const uint8_t prk[32], const char *label, uint8_t out[32]) {
  uint8_t msg[16];
  size_t n = strlen(label);
  memcpy(msg, label, n);
  msg[n] = 0x01;
  hmac_sha256(prk, 32, msg, n + 1, out);
}

// Отправить строку ответа зашифрованной
static void send_data(const char *text) {
  static uint8_t f[CHAN_FRAME_MAX];
  size_t n = strlen(text);
  if (n > sizeof f - HDR - TAG) n = sizeof f - HDR - TAG;
  uint8_t nonce[12];
  chacha20poly1305_ctx ctx;
  f[0] = T_DATA;
  put_be64(f + 1, S.tx_next);
  nonce_of(S.tx_next++, nonce);
  rfc7539_init(&ctx, S.k_tx, nonce);
  rfc7539_auth(&ctx, f, HDR);
  chacha20poly1305_encrypt(&ctx, (const uint8_t *)text, f + HDR, n);
  rfc7539_finish(&ctx, HDR, (int64_t)n, f + HDR + n);
  memzero(&ctx, sizeof ctx);
  g_send(f, HDR + n + TAG);
  memzero(f, HDR + n);
}

static void reply_cb(const char *line) { send_data(line); }

static void on_hello(const uint8_t *f, size_t len) {
  static const uint8_t zero_id[8];
  uint8_t eph[32], dev_pub[32], shared[32], th[32], prk[32], sas[32], bond[32];
  uint8_t ikm[64], mode, id[8], reply[34];
  const uint8_t *host_pub = f + 1, *host_id = f + 33;
  SHA256_CTX c;

  chan_reset();
  if (len != 41) {
    send_error("bad hello");
    return;
  }
  hal_random(eph, sizeof eph);
  curve25519_scalarmult_basepoint(dev_pub, eph);
  curve25519_scalarmult(shared, eph, host_pub);
  memzero(eph, sizeof eph);
  static const uint8_t zero32[32];
  if (!memcmp(shared, zero32, 32)) {  // слабый ключ собеседника
    send_error("bad key");
    return;
  }

  memcpy(ikm, shared, 32);
  size_t ikm_len = 32;
  mode = 0;
  if (memcmp(host_id, zero_id, 8) && bond_find(host_id, ikm + 32)) {
    mode = 1;
    ikm_len = 64;
  }

  sha256_Init(&c);
  static const char LABEL[] = "MicroWallet BLE v1";
  sha256_Update(&c, (const uint8_t *)LABEL, sizeof LABEL - 1);
  sha256_Update(&c, host_pub, 32);
  sha256_Update(&c, dev_pub, 32);
  sha256_Update(&c, host_id, 8);
  sha256_Update(&c, &mode, 1);
  sha256_Final(&c, th);
  hmac_sha256(th, 32, ikm, ikm_len, prk);
  expand(prk, "h2d", S.k_rx);
  expand(prk, "d2h", S.k_tx);
  expand(prk, "sas", sas);
  expand(prk, "bond", bond);
  memzero(prk, sizeof prk);
  memzero(ikm, sizeof ikm);
  memzero(shared, sizeof shared);

  reply[0] = T_HELLO_R;
  memcpy(reply + 1, dev_pub, 32);
  reply[33] = mode;
  g_send(reply, sizeof reply);

  if (mode == 1) {
    S.active = true;
    send_data("ready");
  } else {
    char code[8];
    uint32_t v = ((uint32_t)sas[0] << 24 | (uint32_t)sas[1] << 16 | (uint32_t)sas[2] << 8 | sas[3]) % 1000000;
    snprintf(code, sizeof code, "%03lu %03lu", (unsigned long)(v / 1000), (unsigned long)(v % 1000));
    if (!app_pair_confirm(code)) {
      send_error("rejected");
      chan_reset();
    } else {
      char msg[32];
      hal_random(id, sizeof id);
      if (!bond_add(id, bond)) {
        send_error("bond storage full");
        chan_reset();
      } else {
        S.active = true;
        snprintf(msg, sizeof msg, "paired %02x%02x%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2],
                 id[3], id[4], id[5], id[6], id[7]);
        send_data(msg);
      }
    }
  }
  memzero(sas, sizeof sas);
  memzero(bond, sizeof bond);
}

static void on_data(const uint8_t *f, size_t len) {
  static char line[APP_LINE_MAX];
  uint8_t nonce[12], tag[16];
  chacha20poly1305_ctx ctx;

  if (!S.active) {
    send_error("no session");
    return;
  }
  if (len < HDR + TAG || len - HDR - TAG > sizeof line - 1) {
    send_error("bad frame");
    chan_reset();
    return;
  }
  uint64_t ctr = get_be64(f + 1);
  size_t n = len - HDR - TAG;
  if (ctr < S.rx_next) {  // повтор
    send_error("replay");
    chan_reset();
    return;
  }
  nonce_of(ctr, nonce);
  rfc7539_init(&ctx, S.k_rx, nonce);
  rfc7539_auth(&ctx, f, HDR);
  chacha20poly1305_decrypt(&ctx, f + HDR, (uint8_t *)line, n);
  rfc7539_finish(&ctx, HDR, (int64_t)n, tag);
  memzero(&ctx, sizeof ctx);
  // Сравнение без раннего выхода
  uint8_t diff = 0;
  for (int i = 0; i < TAG; i++) diff |= tag[i] ^ f[HDR + n + i];
  if (diff) {
    memzero(line, n);
    send_error("bad mac");
    chan_reset();
    return;
  }
  S.rx_next = ctr + 1;
  line[n] = 0;
  app_line(line, reply_cb);
  memzero(line, n);
}

void chan_frame(const uint8_t *frame, size_t len, chan_send_fn send) {
  g_send = send;
  if (!len) return;
  switch (frame[0]) {
    case T_HELLO:
      on_hello(frame, len);
      break;
    case T_DATA:
      on_data(frame, len);
      break;
    default:
      send_error("unknown frame");
  }
}
