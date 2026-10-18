// Тестовое приложение: ed25519 (SLIP-10), sr25519 (Substrate), blake2b через прошивку.
//   ed <hardened индексы через пробел>          -> pubkey hex
//   edsign <индексы> ; <msg hex>                 -> подтверждение, подпись hex
//   sr <junctions hex (n×33)>                    -> pubkey hex
//   srsign <junctions hex> <msg hex>             -> подтверждение, подпись hex
//   blake <hex>                                  -> blake2b-256 hex
//   edsoft                                       -> не-hardened путь: ошибка ожидаема
#include "mw.h"

static char out[260];

static int parse_path(const char **p, const char *end, uint32_t path[8]) {
  int n = 0;
  const char *w;
  size_t wl;
  while (n < 8 && (wl = mw_word(p, end, &w)) && w[0] != ';') {
    uint32_t v;
    if (!mw_u32(w, wl, &v)) return -1;
    path[n++] = H(v);
  }
  return n;
}

void mw_main(const char *cmd, size_t len) {
  const char *p = cmd, *end = cmd + len, *w;
  size_t wl = mw_word(&p, end, &w);
  uint8_t pub[32], sig[64], buf[512], j[8 * 33];
  uint32_t path[8];
  if (wl == 2 && !memcmp(w, "ed", 2)) {
    int n = parse_path(&p, end, path);
    int r = mw_ed25519_pubkey(path, n, pub);
    if (r) {
      mw_err(r == -2 ? "denied" : "error");
      return;
    }
    mw_hex(pub, 32, out);
    mw_ok(out);
  } else if (wl == 6 && !memcmp(w, "edsoft", 6)) {
    uint32_t soft[3] = {H(44), H(501), 0};
    mw_ok(mw_ed25519_pubkey(soft, 3, pub) == -3 ? "refused" : "BROKEN");
  } else if (wl == 6 && !memcmp(w, "edsign", 6)) {
    int n = parse_path(&p, end, path);
    const char *h;
    size_t hl = mw_word(&p, end, &h);
    int ml = mw_unhex(h, hl, buf, sizeof buf);
    if (ml < 0) {
      mw_err("bad hex");
      return;
    }
    mw_field("Message", "test");
    if (!mw_confirm("ed25519 test")) {
      mw_err("rejected");
      return;
    }
    if (mw_ed25519_sign(path, n, buf, (size_t)ml, sig)) {
      mw_err("sign failed");
      return;
    }
    mw_hex(sig, 64, out);
    mw_ok(out);
  } else if (wl == 2 && !memcmp(w, "sr", 2)) {
    const char *h;
    size_t hl = mw_word(&p, end, &h);
    int jl = hl ? mw_unhex(h, hl, j, sizeof j) : 0;
    if (jl < 0 || jl % 33) {
      mw_err("bad junctions");
      return;
    }
    int r = mw_sr25519_pubkey(j, jl / 33, pub);
    if (r) {
      mw_err(r == -2 ? "denied" : "error");
      return;
    }
    mw_hex(pub, 32, out);
    mw_ok(out);
  } else if (wl == 6 && !memcmp(w, "srsign", 6)) {
    const char *h, *m;
    size_t hl = mw_word(&p, end, &h), ml = mw_word(&p, end, &m);
    int jl = mw_unhex(h, hl, j, sizeof j), n = mw_unhex(m, ml, buf, sizeof buf);
    if (jl < 0 || jl % 33 || n < 0) {
      mw_err("bad hex");
      return;
    }
    mw_field("Message", "test");
    if (!mw_confirm("sr25519 test")) {
      mw_err("rejected");
      return;
    }
    if (mw_sr25519_sign(j, jl / 33, buf, (size_t)n, sig)) {
      mw_err("sign failed");
      return;
    }
    mw_hex(sig, 64, out);
    mw_ok(out);
  } else if (wl == 5 && !memcmp(w, "blake", 5)) {
    const char *h;
    size_t hl = mw_word(&p, end, &h);
    int n = mw_unhex(h, hl, buf, sizeof buf);
    if (n < 0 || mw_blake2b(buf, (size_t)n, 32, pub)) {
      mw_err("bad");
      return;
    }
    mw_hex(pub, 32, out);
    mw_ok(out);
  } else {
    mw_err("unknown command");
  }
}
