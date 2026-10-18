// apps.c — приложения на WebAssembly: хранение, установка, запуск и API для них.
// Описание формата и протокола — в apps.h.
#include "apps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blake2b.h"
#include "coins.h"
#include "hal.h"
#include "icons.h"
#include "memzero.h"
#include "menu.h"
#include "sha2.h"
#include "sha3.h"
#include "oled.h"
#include "ui.h"
#include "ux.h"
#include "wallet.h"
#include "wasm_export.h"

#define HDR_SIZE 512
#define APP_MAX (64 * 1024 - HDR_SIZE)  // модуль целиком копируется в RAM при запуске
#define MAGIC "MWAPP2"  // 6 символов + 0, поле — 8 байт
#define MAX_APPS 64  // в RAM — список с манифестами (~150 байт на приложение)
#define BUF_SIZE 4096  // буфер команды в памяти модуля (mw_buf)
#define WASM_STACK (8 * 1024)  // стек интерпретатора (операнды и кадры)
// Бюджет инструкций на один вызов приложения: бесконечный цикл не подвесит устройство.
// Считаются только инструкции WebAssembly — ожидание кнопки и подписи в прошивке не в счёт.
// Замер в симуляторе: подпись Pearl с 16 входами ~2.6 млн, BTC 16 входов ~1.5 млн, адрес — тысячи.
#define APP_MAX_INSTR 50000000

// Раздел apps: приложения подряд, каждое — с границы сектора (4 КБ): заголовок 512 байт
// (tools/mkapps.py пишет такой же) + модуль. Свободное место — стёртые секторы.
// Манифест хранится и в заголовке, чтобы список читался без самих модулей.
typedef struct {
  char magic[8];
  uint32_t len;  // little-endian, как и обе платформы
  uint32_t seq;
  uint8_t sha[32];
  char manifest[HDR_SIZE - 8 - 4 - 4 - 32];
} slot_hdr_t;
_Static_assert(sizeof(slot_hdr_t) == HDR_SIZE, "app header must be 512 bytes");

static mwapp_t g_apps[MAX_APPS];
static int g_count;

static uint32_t span_of(uint32_t len) {
  return (HDR_SIZE + len + HAL_APPS_SECTOR - 1) / HAL_APPS_SECTOR * HAL_APPS_SECTOR;
}

// ---------- манифест ----------

static bool leb(const uint8_t *p, size_t n, size_t *i, uint32_t *v) {
  uint32_t r = 0;
  for (int sh = 0; *i < n && sh < 35; sh += 7) {
    uint8_t b = p[(*i)++];
    r |= (uint32_t)(b & 0x7F) << sh;
    if (!(b & 0x80)) {
      *v = r;
      return true;
    }
  }
  return false;
}

// Пользовательская секция "microwallet" модуля
static const char *find_manifest(const uint8_t *w, size_t n, size_t *len) {
  static const char NAME[] = "microwallet";
  if (n < 8 || memcmp(w, "\0asm\1\0\0\0", 8)) return NULL;
  size_t i = 8;
  while (i < n) {
    uint8_t id = w[i++];
    uint32_t sz, nl;
    if (!leb(w, n, &i, &sz) || sz > n - i) return NULL;
    size_t end = i + sz, j = i;
    if (id == 0 && leb(w, end, &j, &nl) && nl == sizeof NAME - 1 && nl <= end - j &&
        !memcmp(w + j, NAME, nl)) {
      *len = end - (j + nl);
      return (const char *)w + j + nl;
    }
    i = end;
  }
  return NULL;
}

static int unhex_n(const char *s, uint8_t *out, int n) {
  for (int i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return n;
}

static bool parse_path(const char *s, size_t n, mwapp_t *a) {
  if (a->npaths >= APP_PATHS) return false;
  int d = 0;
  size_t i = 0;
  while (i < n) {
    uint32_t v = 0;
    size_t start = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    if (i == start || v >= 0x80000000u || d >= APP_PATH_DEPTH) return false;
    if (i < n && (s[i] == '\'' || s[i] == 'h')) {
      v |= 0x80000000u;
      i++;
    }
    a->paths[a->npaths][d++] = v;
    if (i < n && s[i] != '/') return false;
    i++;
  }
  // Свой путь у каждого приложения, но не корень и не чужие ветки встроенных монет:
  // первый уровень должен быть hardened
  if (d < 2 || !(a->paths[a->npaths][0] & 0x80000000u)) return false;
  a->path_len[a->npaths++] = d;
  return true;
}

static bool parse_manifest(const char *m, size_t n, mwapp_t *a, const char **err) {
  memset(a, 0, sizeof *a);
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && m[e] != '\n' && m[e] != 0) e++;
    const char *line = m + i, *eq = memchr(line, '=', e - i);
    if (eq) {
      size_t kl = eq - line, vl = e - i - kl - 1;
      const char *v = eq + 1;
#define KEY(k) (kl == sizeof k - 1 && !memcmp(line, k, kl))
      if (KEY("id") && vl >= 2 && vl < sizeof a->id) memcpy(a->id, v, vl);
      else if (KEY("name") && vl < sizeof a->name) memcpy(a->name, v, vl);
      else if (KEY("version") && vl < sizeof a->version) memcpy(a->version, v, vl);
      else if (KEY("icon") && vl == 16) unhex_n(v, a->icon, 8);
      else if (KEY("sr25519") && vl == 3 && !memcmp(v, "yes", 3)) a->sr25519 = true;
      else if (KEY("path") && !parse_path(v, vl, a)) {
        *err = "bad path in manifest";
        return false;
      }
#undef KEY
    }
    i = e + 1;
  }
  for (const char *p = a->id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) a->id[0] = 0;
  if (!a->id[0] || !a->name[0] || !a->version[0]) {
    *err = "manifest needs id, name, version";
    return false;
  }
  return true;
}

void apps_paths_str(const mwapp_t *a, char *out, int outlen) {
  int o = 0;
  out[0] = 0;
  for (int p = 0; p < a->npaths && o < outlen - 1; p++) {
    if (p) o += snprintf(out + o, outlen - o, ", ");
    for (int d = 0; d < a->path_len[p] && o < outlen - 1; d++) {
      uint32_t v = a->paths[p][d];
      o += snprintf(out + o, outlen - o, "%s%lu%s", d ? "/" : "", (unsigned long)(v & 0x7FFFFFFFu),
                    v & 0x80000000u ? "'" : "");
    }
  }
  if (a->sr25519 && o < outlen - 1) snprintf(out + o, outlen - o, "%ssr25519", o ? ", " : "");
  if (!a->npaths && !a->sr25519) snprintf(out, outlen, "none");
}

// ---------- хранилище ----------

void apps_init(void) {
  uint32_t size = hal_apps_size(), off = 0;
  g_count = 0;
  while (off + HDR_SIZE <= size && g_count < MAX_APPS) {
    static slot_hdr_t h;
    const char *err;
    mwapp_t *a = &g_apps[g_count];
    if (!hal_apps_read(off, &h, sizeof h) || memcmp(h.magic, MAGIC, sizeof MAGIC) || h.len > APP_MAX ||
        off + span_of(h.len) > size) {
      off += HAL_APPS_SECTOR;
      continue;
    }
    h.manifest[sizeof h.manifest - 1] = 0;
    if (parse_manifest(h.manifest, strlen(h.manifest), a, &err)) {
      a->off = off;
      a->span = span_of(h.len);
      a->seq = h.seq;
      // Две копии одного приложения — питание пропало посреди обновления: старую стираем
      int dup = -1;
      for (int i = 0; i < g_count; i++)
        if (!strcmp(g_apps[i].id, a->id)) dup = i;
      if (dup < 0) {
        g_count++;
      } else {
        mwapp_t *old = &g_apps[dup], *stale = old->seq > a->seq ? a : old;
        hal_apps_erase(stale->off, stale->span);
        if (stale == old) *old = *a;
      }
    }
    off += span_of(h.len);
  }
}

uint32_t apps_total(void) { return hal_apps_size(); }

uint32_t apps_free(void) {
  uint32_t used = 0;
  for (int i = 0; i < g_count; i++) used += g_apps[i].span;
  return hal_apps_size() > used ? hal_apps_size() - used : 0;
}

int apps_count(void) { return g_count; }
const mwapp_t *apps_get(int i) { return i >= 0 && i < g_count ? &g_apps[i] : NULL; }

static const mwapp_t *find(const char *id) {
  for (int i = 0; id && i < g_count; i++)
    if (!strcmp(g_apps[i].id, id)) return &g_apps[i];
  return NULL;
}

// Первое свободное место нужного размера (секторы, не занятые ни одним приложением)
static long free_space(uint32_t need) {
  uint32_t size = hal_apps_size();
  for (uint32_t off = 0; off + need <= size;) {
    uint32_t next = off;
    for (int i = 0; i < g_count; i++)
      if (g_apps[i].off < off + need && off < g_apps[i].off + g_apps[i].span && g_apps[i].off + g_apps[i].span > next)
        next = g_apps[i].off + g_apps[i].span;
    if (next == off) return (long)off;
    off = next;
  }
  return -1;
}

// ---------- запуск ----------

static const mwapp_t *g_app;  // текущее приложение
static app_reply_fn g_out;
static bool g_replied, g_ok, g_confirmed, g_signing;
static int g_ui;  // 0 — экранов не было, 1 — проверка транзакции, 2 — сверка адреса
static char g_err[64];

// Шаги экрана проверки: поле, сумма крупно, предупреждение
#define FIELDS 40  // до 16 получателей × (сумма + адрес) + предупреждение + комиссия
static char g_ftitle[FIELDS][24], g_ftext[FIELDS][100];
static ux_kind_t g_fkind[FIELDS];
static const uint8_t *g_ficon[FIELDS];
static int g_nfields;

// ---------- большие данные: вход «@» и длинный ответ ----------
// Строка команды — до APP_LINE_MAX, поэтому большой вход (транзакция с prev tx, PSBT)
// загружается заранее блоком в PSRAM (app put …) и отдаётся вызову, который кончается
// на « @». Ответ приложения копится в буфере: короткий уходит строкой, длинный —
// «ok @<размер> <sha256>», и хост забирает его кусками (app get <смещение>).
#define BLOB_MAX (512 * 1024)
#define RBUF_MAX (256 * 1024)
#define LINE_REPLY 3000
static uint8_t *g_blob, g_blob_sha[32];
static uint32_t g_blob_len, g_blob_got;
static bool g_blob_ready, g_blob_on;  // загружен и проверен / доступен текущему вызову
static char *g_rbuf, *g_big;          // ответ текущего вызова / длинный ответ для app get
static uint32_t g_rlen, g_rcap, g_big_len;
static bool g_buffered;  // приложение ответило «ok», ответ ещё в буфере

static void blob_reset(void) {
  free(g_blob);
  g_blob = NULL;
  g_blob_len = g_blob_got = 0;
  g_blob_ready = false;
}

static void big_reset(void) {
  free(g_big);
  g_big = NULL;
  g_big_len = 0;
}

static bool rbuf_add(const char *s, uint32_t len) {
  if (g_rlen + len > RBUF_MAX) return false;
  if (g_rlen + len > g_rcap) {
    uint32_t cap = g_rcap ? g_rcap * 2 : 1024;
    while (cap < g_rlen + len) cap *= 2;
    char *n = realloc(g_rbuf, cap);
    if (!n) return false;
    g_rbuf = n;
    g_rcap = cap;
  }
  memcpy(g_rbuf + g_rlen, s, len);
  g_rlen += len;
  return true;
}

static void rbuf_reset(void) {
  free(g_rbuf);
  g_rbuf = NULL;
  g_rlen = g_rcap = 0;
  g_buffered = false;
}

static void out_text(const char *prefix, const char *s, uint32_t len) {
  static char line[4096];
  if (g_replied) return;
  g_replied = true;
  g_ok = !strncmp(prefix, "ok", 2);
  if (!g_ok) snprintf(g_err, sizeof g_err, "%.*s", (int)(len < sizeof g_err - 1 ? len : sizeof g_err - 1), s);
  if (len > sizeof line - 8) len = sizeof line - 8;
  int n = snprintf(line, sizeof line, "%s", prefix);
  memcpy(line + n, s, len);
  line[n + len] = 0;
  for (char *p = line; *p; p++)
    if (*p == '\n' || *p == '\r') *p = ' ';  // ответ — одна строка
  g_out(line);
}

static void reply_flush(void) {
  if (!g_buffered) return;
  g_buffered = false;
  g_replied = false;
  for (uint32_t i = 0; i < g_rlen; i++)
    if (g_rbuf[i] == '\n' || g_rbuf[i] == '\r') g_rbuf[i] = ' ';  // ответ — одна строка
  if (g_rlen <= LINE_REPLY) {
    out_text("ok ", g_rbuf ? g_rbuf : "", g_rlen);
  } else {
    char line[96], hex[65];
    uint8_t sha[32];
    big_reset();
    g_big = g_rbuf;
    g_big_len = g_rlen;
    g_rbuf = NULL;
    g_rlen = g_rcap = 0;
    sha256_Raw((const uint8_t *)g_big, g_big_len, sha);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    snprintf(line, sizeof line, "@%lu %s", (unsigned long)g_big_len, hex);
    out_text("ok ", line, strlen(line));
  }
  rbuf_reset();
}

// Первая подпись после «Accept»: экран, пока считается (у BTC до 16 входов)
static void signing_screen(void) {
  if (g_signing) return;
  g_signing = true;
  ui_message(ICON_PEN, "Signing", "please wait");
  oled_show();
}

static void *mem(wasm_exec_env_t e, uint32_t off, uint32_t len) {
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
  if (!wasm_runtime_validate_app_addr(inst, off, len)) return NULL;  // исключение уже стоит
  return wasm_runtime_addr_app_to_native(inst, off);
}

static void copy_str(wasm_exec_env_t e, uint32_t off, uint32_t len, char *out, size_t outlen) {
  const char *s = mem(e, off, len);
  out[0] = 0;
  if (!s) return;
  if (len > outlen - 1) len = outlen - 1;
  memcpy(out, s, len);
  out[len] = 0;
}

static bool path_allowed(const uint32_t *p, uint32_t n) {
  if (!g_app || n < 1 || n > WALLET_PATH_MAX) return false;
  for (int i = 0; i < g_app->npaths; i++) {
    int d = g_app->path_len[i];
    if ((int)n >= d && !memcmp(p, g_app->paths[i], d * sizeof *p)) return true;
  }
  return false;
}

static void n_reply(wasm_exec_env_t e, uint32_t off, uint32_t len) {
  const char *s = mem(e, off, len);
  if (!s || g_replied) return;  // отвечают один раз
  g_replied = g_ok = g_buffered = true;
  if (!rbuf_add(s, len)) wasm_runtime_set_exception(wasm_runtime_get_module_inst(e), "reply too long");
}

static void n_reply_more(wasm_exec_env_t e, uint32_t off, uint32_t len) {
  const char *s = mem(e, off, len);
  if (!s || !g_buffered) return;  // только после mw_reply
  if (!rbuf_add(s, len)) wasm_runtime_set_exception(wasm_runtime_get_module_inst(e), "reply too long");
}

static int32_t n_blob_size(wasm_exec_env_t e) {
  (void)e;
  return g_blob_on ? (int32_t)g_blob_len : -1;
}

static int32_t n_blob_read(wasm_exec_env_t e, uint32_t off, uint32_t dst, uint32_t len) {
  if (!g_blob_on || off > g_blob_len) return -1;
  if (len > g_blob_len - off) len = g_blob_len - off;
  void *d = mem(e, dst, len);
  if (!d) return -1;
  memcpy(d, g_blob + off, len);
  return (int32_t)len;
}

// SHA-256 (dbl — двойной) от склейки n кусков блока: пары u32 (смещение, длина). Нужно,
// например, для txid предыдущей транзакции без witness — хешировать в wasm долго.
static int32_t n_blob_sha256(wasm_exec_env_t e, uint32_t ranges, uint32_t n, uint32_t dbl, uint32_t out) {
  if (!g_blob_on || n < 1 || n > 64) return -1;
  const uint8_t *r = mem(e, ranges, n * 8);
  uint8_t *o = mem(e, out, 32);
  if (!r || !o) return -1;
  SHA256_CTX c;
  sha256_Init(&c);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t off, len;
    memcpy(&off, r + 8 * i, 4);  // память модуля — little-endian, как и обе платформы
    memcpy(&len, r + 8 * i + 4, 4);
    if (off > g_blob_len || len > g_blob_len - off) return -1;
    sha256_Update(&c, g_blob + off, len);
  }
  sha256_Final(&c, o);
  if (dbl) sha256_Raw(o, 32, o);
  return 0;
}

static void n_fail(wasm_exec_env_t e, uint32_t off, uint32_t len) {
  const char *s = mem(e, off, len);
  if (s) out_text("err ", s, len);
}

static int32_t n_net(wasm_exec_env_t e) {
  (void)e;
#ifdef MW_MAINNET
  return 1;
#else
  return 0;
#endif
}

static int32_t n_pubkey(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4);
  uint8_t *out = mem(e, out_off, 65);
  if (!p || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);  // wasm — little-endian, как и обе платы
  if (!path_allowed(path, n)) return -2;
  return wallet_path_key(path, n, NULL, out) ? 0 : -3;
}

static int32_t n_sign(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t hash_off, uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4), *h = mem(e, hash_off, 32);
  uint8_t *out = mem(e, out_off, 65);
  if (!p || !h || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  if (!g_confirmed) return -4;  // подпись — только после «Accept» в этом вызове
  signing_screen();
  return wallet_path_sign(path, n, h, out, out + 64) ? 0 : -3;
}

static int32_t n_hash160(wasm_exec_env_t e, uint32_t off, uint32_t len, uint32_t out_off) {
  const uint8_t *d = mem(e, off, len);
  uint8_t *out = mem(e, out_off, 20);
  if (!d || !out) return -1;
  hash160(d, len, out);
  return 0;
}

static int32_t n_xpub(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t version, uint32_t out_off,
                      uint32_t out_len) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4);
  char *out = mem(e, out_off, out_len);
  char x[120];
  if (!p || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  if (!wallet_path_xpub(path, n, version, x, sizeof x) || strlen(x) + 1 > out_len) return -3;
  memcpy(out, x, strlen(x) + 1);
  return (int32_t)strlen(x);
}

static int32_t n_taproot_key(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4);
  uint8_t *out = mem(e, out_off, 32);
  if (!p || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  return wallet_path_taproot(path, n, out) ? 0 : -3;
}

static int32_t n_taproot_sign(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t msg_off, uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4), *m = mem(e, msg_off, 32);
  uint8_t *out = mem(e, out_off, 64), aux[32];
  if (!p || !m || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  if (!g_confirmed) return -4;
  signing_screen();
  hal_random(aux, sizeof aux);
  return wallet_path_taproot_sign(path, n, m, aux, out) ? 0 : -3;
}

// ---- ed25519 (SLIP-10) ----

static int32_t n_ed25519_pubkey(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4);
  uint8_t *out = mem(e, out_off, 32);
  if (!p || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  return wallet_ed25519_pubkey(path, n, out) ? 0 : -3;  // -3 и для не-hardened уровня
}

static int32_t n_ed25519_sign(wasm_exec_env_t e, uint32_t path_off, uint32_t n, uint32_t msg_off, uint32_t msg_len,
                              uint32_t out_off) {
  uint32_t path[WALLET_PATH_MAX];
  const uint8_t *p = mem(e, path_off, n * 4), *m = mem(e, msg_off, msg_len);
  uint8_t *out = mem(e, out_off, 64);
  if (!p || !m || !out || n > WALLET_PATH_MAX) return -1;
  memcpy(path, p, n * 4);
  if (!path_allowed(path, n)) return -2;
  if (!g_confirmed) return -4;
  signing_screen();
  return wallet_ed25519_sign(path, n, m, msg_len, out) ? 0 : -3;
}

// ---- sr25519 (Substrate): junctions — n × 33 байт [hard 1/0][chain code 32] ----

static int32_t n_sr25519_pubkey(wasm_exec_env_t e, uint32_t j_off, uint32_t n, uint32_t out_off) {
  const uint8_t *j = mem(e, j_off, n * 33);
  uint8_t *out = mem(e, out_off, 32);
  if (!j || !out || n > WALLET_PATH_MAX) return -1;
  if (!g_app || !g_app->sr25519) return -2;  // нет sr25519=yes в манифесте
  return wallet_sr25519_pubkey(j, n, out) ? 0 : -3;
}

static int32_t n_sr25519_sign(wasm_exec_env_t e, uint32_t j_off, uint32_t n, uint32_t msg_off, uint32_t msg_len,
                              uint32_t out_off) {
  const uint8_t *j = mem(e, j_off, n * 33), *m = mem(e, msg_off, msg_len);
  uint8_t *out = mem(e, out_off, 64);
  if (!j || !m || !out || n > WALLET_PATH_MAX) return -1;
  if (!g_app || !g_app->sr25519) return -2;
  if (!g_confirmed) return -4;
  signing_screen();
  return wallet_sr25519_sign(j, n, m, msg_len, out) ? 0 : -3;
}

static int32_t n_blake2b(wasm_exec_env_t e, uint32_t off, uint32_t len, uint32_t outlen, uint32_t out_off) {
  const uint8_t *d = mem(e, off, len);
  uint8_t *out = mem(e, out_off, outlen);
  if (!d || !out || outlen < 1 || outlen > 64) return -1;
  return blake2b(d, len, out, outlen) == 0 ? 0 : -1;
}

static void n_keccak(wasm_exec_env_t e, uint32_t off, uint32_t len, uint32_t out_off) {
  const uint8_t *d = mem(e, off, len);
  uint8_t *out = mem(e, out_off, 32);
  if (d && out) keccak_256(d, len, out);
}

static void n_sha256(wasm_exec_env_t e, uint32_t off, uint32_t len, uint32_t out_off) {
  const uint8_t *d = mem(e, off, len);
  uint8_t *out = mem(e, out_off, 32);
  if (d && out) sha256_Raw(d, len, out);
}

static void add_step(wasm_exec_env_t e, ux_kind_t kind, uint32_t t_off, uint32_t t_len, uint32_t v_off,
                     uint32_t v_len) {
  if (g_nfields >= FIELDS) return;
  copy_str(e, t_off, t_len, g_ftitle[g_nfields], sizeof g_ftitle[0]);
  copy_str(e, v_off, v_len, g_ftext[g_nfields], sizeof g_ftext[0]);
  const char *t = g_ftitle[g_nfields];
  g_fkind[g_nfields] = kind;
  g_ficon[g_nfields] = kind == UX_TITLE                                    ? ICON_WARN
                       : !strncmp(t, "To", 2)                              ? ICON_SEND
                       : (strstr(t, "fee") || strstr(t, "Fee"))           ? ICON_FEE
                       : kind == UX_AMOUNT                                 ? ICON_COIN
                                                                           : ICON_INFO;
  g_nfields++;
}

static void n_field(wasm_exec_env_t e, uint32_t t_off, uint32_t t_len, uint32_t v_off, uint32_t v_len) {
  add_step(e, UX_FIELD, t_off, t_len, v_off, v_len);
}

static void n_amount(wasm_exec_env_t e, uint32_t t_off, uint32_t t_len, uint32_t v_off, uint32_t v_len) {
  add_step(e, UX_AMOUNT, t_off, t_len, v_off, v_len);
}

static void n_warn(wasm_exec_env_t e, uint32_t t_off, uint32_t t_len, uint32_t v_off, uint32_t v_len) {
  add_step(e, UX_TITLE, t_off, t_len, v_off, v_len);
}

// Экран проверки: «Review <что>», шаги, Accept / Reject
static int32_t n_confirm(wasm_exec_env_t e, uint32_t t_off, uint32_t t_len) {
  static char title[24];
  ux_step_t st[FIELDS + 3];
  int n = 0;
  copy_str(e, t_off, t_len, title, sizeof title);
  st[n++] = (ux_step_t){UX_TITLE, ICON_EYE, "Review", title};
  for (int i = 0; i < g_nfields; i++) st[n++] = (ux_step_t){g_fkind[i], g_ficon[i], g_ftitle[i], g_ftext[i]};
  st[n++] = (ux_step_t){UX_ACCEPT, ICON_CHECK, "Accept", "hold to sign"};
  st[n++] = (ux_step_t){UX_REJECT, ICON_CROSS, "Reject", "hold to cancel"};
  g_nfields = 0;
  g_ui = 1;
  bool ok = ux_review(st, n);
  g_confirmed |= ok;
  return ok;
}

static int32_t n_address(wasm_exec_env_t e, uint32_t t_off, uint32_t t_len, uint32_t a_off, uint32_t a_len) {
  static char title[24], addr[128];
  copy_str(e, t_off, t_len, title, sizeof title);
  copy_str(e, a_off, a_len, addr, sizeof addr);
  const ux_step_t st[] = {
      {UX_TITLE, ICON_EYE, "Verify", "address"},
      {UX_FIELD, g_app->icon, title, addr},
      {UX_ACCEPT, ICON_CHECK, "Approve", "hold if it matches"},
      {UX_REJECT, ICON_CROSS, "Reject", "hold if it differs"},
  };
  g_ui = 2;
  return ux_review(st, 4);
}

static NativeSymbol g_natives[] = {
    {"reply", n_reply, "(ii)", NULL},
    {"fail", n_fail, "(ii)", NULL},
    {"net", n_net, "()i", NULL},
    {"pubkey", n_pubkey, "(iii)i", NULL},
    {"sign", n_sign, "(iiii)i", NULL},
    {"keccak256", n_keccak, "(iii)", NULL},
    {"sha256", n_sha256, "(iii)", NULL},
    {"hash160", n_hash160, "(iii)i", NULL},
    {"xpub", n_xpub, "(iiiii)i", NULL},
    {"taproot_key", n_taproot_key, "(iii)i", NULL},
    {"taproot_sign", n_taproot_sign, "(iiii)i", NULL},
    {"ed25519_pubkey", n_ed25519_pubkey, "(iii)i", NULL},
    {"ed25519_sign", n_ed25519_sign, "(iiiii)i", NULL},
    {"sr25519_pubkey", n_sr25519_pubkey, "(iii)i", NULL},
    {"sr25519_sign", n_sr25519_sign, "(iiiii)i", NULL},
    {"blake2b", n_blake2b, "(iiii)i", NULL},
    {"ui_field", n_field, "(iiii)", NULL},
    {"ui_amount", n_amount, "(iiii)", NULL},
    {"ui_warn", n_warn, "(iiii)", NULL},
    {"ui_confirm", n_confirm, "(ii)i", NULL},
    {"ui_address", n_address, "(iiii)i", NULL},
    {"reply_more", n_reply_more, "(ii)", NULL},
    {"blob_size", n_blob_size, "()i", NULL},
    {"blob_read", n_blob_read, "(iii)i", NULL},
    {"blob_sha256", n_blob_sha256, "(iiii)i", NULL},
};

// Память движка. Свой распределитель вместо системного: os_realloc в порте WAMR для
// ESP-IDF при переносе блока пересчитывает выравнивание заново, и данные сдвигаются
// на 4 байта (malloc там выравнивает на 4) — загрузчик видит мусор вместо стека
// типов: «type mismatch». Здесь перед блоком хранятся размер и исходный адрес,
// а realloc — всегда новый блок и копия.
#define WMEM_HDR (2 * sizeof(uintptr_t))

static void *wmem_malloc(unsigned size) {
  uint8_t *o = malloc(size + WMEM_HDR + 8);
  if (!o) return NULL;
  uint8_t *p = (uint8_t *)(((uintptr_t)o + WMEM_HDR + 7) & ~(uintptr_t)7);  // выравнивание 8
  ((uintptr_t *)p)[-1] = (uintptr_t)o;
  ((uintptr_t *)p)[-2] = size;
  return p;
}

static void wmem_free(void *p) {
  if (p) free((void *)((uintptr_t *)p)[-1]);
}

static void *wmem_realloc(void *p, unsigned size) {
  if (!p) return wmem_malloc(size);
  uint8_t *n = wmem_malloc(size);
  if (!n) return NULL;
  unsigned old = (unsigned)((uintptr_t *)p)[-2];
  memcpy(n, p, old < size ? old : size);
  wmem_free(p);
  return n;
}

static bool runtime_init(void) {
  static bool ready;
  if (ready) return true;
  RuntimeInitArgs init;
  memset(&init, 0, sizeof init);
  init.mem_alloc_type = Alloc_With_Allocator;
  init.mem_alloc_option.allocator.malloc_func = (void *)wmem_malloc;
  init.mem_alloc_option.allocator.realloc_func = (void *)wmem_realloc;
  init.mem_alloc_option.allocator.free_func = (void *)wmem_free;
  init.native_module_name = "mw";
  init.native_symbols = g_natives;
  init.n_native_symbols = sizeof g_natives / sizeof *g_natives;
  ready = wasm_runtime_full_init(&init);
  return ready;
}

static int instr_limit(void) {
#ifdef MW_SIM
  const char *e = getenv("MW_SIM_INSTR");  // для замеров: сколько нужно легальным командам
  if (e) return atoi(e);
#endif
  return APP_MAX_INSTR;
}

static void run_here(const mwapp_t *a, const char *cmd, app_reply_fn out) {
  static slot_hdr_t h;
  char err[128];
  uint8_t sha[32];
  g_out = out;
  g_replied = g_ok = g_confirmed = g_signing = false;
  g_ui = 0;
  g_nfields = 0;
  g_app = a;

  if (!runtime_init()) {
    out_text("err ", "wasm runtime init failed", 24);
    return;
  }
  if (!hal_apps_read(a->off, &h, sizeof h)) {
    out_text("err ", "app read failed", 15);
    return;
  }
  uint8_t *code = malloc(h.len);
  if (!code) {
    out_text("err ", "out of memory", 13);
    return;
  }
  wasm_module_t m = NULL;
  wasm_module_inst_t inst = NULL;
  wasm_exec_env_t env = NULL;
  if (!hal_apps_read(a->off + HDR_SIZE, code, h.len)) {
    out_text("err ", "app read failed", 15);
    goto done;
  }
  sha256_Raw(code, h.len, sha);
  if (memcmp(sha, h.sha, 32)) {  // флеш повреждён — такой код не запускаем
    out_text("err ", "app corrupted, reinstall it", 27);
    goto done;
  }
  if (!(m = wasm_runtime_load(code, h.len, err, sizeof err)) ||
      !(inst = wasm_runtime_instantiate(m, WASM_STACK, 0, err, sizeof err)) ||
      !(env = wasm_runtime_create_exec_env(inst, WASM_STACK))) {
    uint32_t hf, hl;  // для диагностики: сколько памяти было
    hal_heap(&hf, &hl);
    snprintf(err + strlen(err), sizeof err - strlen(err), " (free %luK, block %luK)", (unsigned long)hf / 1024,
             (unsigned long)hl / 1024);
    out_text("err app load: ", err, strlen(err));
    goto done;
  }
  wasm_runtime_set_instruction_count_limit(env, instr_limit());
  wasm_function_inst_t fbuf = wasm_runtime_lookup_function(inst, "mw_buf");
  wasm_function_inst_t fcmd = wasm_runtime_lookup_function(inst, "mw_cmd");
  uint32_t argv[1] = {0};
  size_t len = strlen(cmd);
  if (!fbuf || !fcmd || len >= BUF_SIZE) {
    out_text("err ", "not a MicroWallet app", 21);
    goto done;
  }
  if (!wasm_runtime_call_wasm(env, fbuf, 0, argv) || !wasm_runtime_validate_app_addr(inst, argv[0], len + 1)) {
    out_text("err ", "bad mw_buf", 10);
    goto done;
  }
  char *dst = wasm_runtime_addr_app_to_native(inst, argv[0]);
  memcpy(dst, cmd, len + 1);
  argv[0] = (uint32_t)len;
  if (!wasm_runtime_call_wasm(env, fcmd, 1, argv)) {
    const char *x = wasm_runtime_get_exception(inst);
    if (g_buffered) {  // ответило, а потом упало — такой ответ не отдаём
      rbuf_reset();
      g_replied = g_ok = false;
    }
    out_text("err app crashed: ", x ? x : "?", x ? strlen(x) : 1);
    if (x && strstr(x, "instruction limit")) {  // зациклилось или считает слишком долго
      ux_flash(ICON_WARN, "App stopped", "took too long", 1500);
      menu_draw();
    }
  } else if (g_buffered) {
    reply_flush();
  } else {
    out_text("err ", "app gave no answer", 18);  // ничего не ответило (иначе g_replied)
  }

done:
  rbuf_reset();
  if (env) wasm_runtime_destroy_exec_env(env);
  if (inst) wasm_runtime_deinstantiate(inst);
  if (m) wasm_runtime_unload(m);
  memzero(code, h.len);
  free(code);
  g_app = NULL;
  g_confirmed = false;
  // Итог экранов — как у встроенных монет раньше
  if (g_ui) {
    if (g_ok) ux_flash(ICON_CHECK, g_ui == 1 ? "Signed" : "Approved", NULL, g_ui == 1 ? 900 : 700);
    else if (!strcmp(g_err, "rejected")) ux_flash(ICON_CROSS, "Rejected", NULL, 900);
    else ux_error(g_err);
    menu_draw();
  }
}

// WAMR вызывает pthread_self(), а в ESP-IDF это работает только внутри pthread —
// у обычной задачи FreeRTOS (app_main) он падает на assert. Поэтому приложение
// выполняется в своём потоке; вызывающий ждёт его, так что экран и кнопка, как и
// раньше, принадлежат приложению целиком.
#ifdef MW_SIM
#define APP_THREAD_STACK (256 * 1024)  // Linux x86-64: WAMR забирает часть под защитные страницы
#else
#define APP_THREAD_STACK (20 * 1024)  // как у app_main: подписи, деривация, экран
#endif

typedef struct {
  const mwapp_t *a;
  const char *cmd;
  app_reply_fn out;
} run_args_t;

static void *run_thread(void *p) {
  const run_args_t *r = p;
  wasm_runtime_init_thread_env();
  run_here(r->a, r->cmd, r->out);
  wasm_runtime_destroy_thread_env();
  return NULL;
}

static void run(const mwapp_t *a, const char *cmd, app_reply_fn out) {
  run_args_t r = {a, cmd, out};
  pthread_attr_t attr;
  pthread_t t;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, APP_THREAD_STACK);
  int rc = pthread_create(&t, &attr, run_thread, &r);
  pthread_attr_destroy(&attr);
  if (rc) {
    out_text("err ", "out of memory", 13);
    return;
  }
  pthread_join(t, NULL);
}

static char *g_query_out;
static int g_query_len;
static bool g_query_ok;

static void query_reply(const char *line) {
  g_query_ok = !strncmp(line, "ok", 2);
  snprintf(g_query_out, g_query_len, "%s", line + (g_query_ok ? 3 : 4));
}

bool apps_query(const mwapp_t *a, const char *cmd, char *out, int outlen) {
  g_query_out = out;
  g_query_len = outlen;
  g_query_ok = false;
  run(a, cmd, query_reply);
  return g_query_ok;
}

// ---------- установка и удаление ----------

static uint8_t *g_inst;  // модуль, который сейчас передают
static uint32_t g_inst_len, g_inst_got;
static uint8_t g_inst_sha[32];

static void inst_reset(void) {
  free(g_inst);
  g_inst = NULL;
  g_inst_len = g_inst_got = 0;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static long unb64(const char *s, uint8_t *out, size_t max) {
  size_t n = s ? strlen(s) : 0, o = 0;
  if (n % 4) return -1;
  for (size_t i = 0; i < n; i += 4) {
    int v[4], pad = 0;
    for (int k = 0; k < 4; k++) {
      if (s[i + k] == '=' && i + 4 == n && k >= 2) {
        v[k] = 0;
        pad++;
      } else if (pad || (v[k] = b64v(s[i + k])) < 0) {
        return -1;
      }
    }
    uint32_t x = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
    const uint8_t b[3] = {(uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x};
    for (int k = 0; k < 3 - pad; k++) {
      if (o >= max) return -1;
      out[o++] = b[k];
    }
  }
  return (long)o;
}

static void b64enc(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t x = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = T[x >> 18 & 63];
    out[o++] = T[x >> 12 & 63];
    out[o++] = i + 1 < n ? T[x >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? T[x & 63] : '=';
  }
  out[o] = 0;
}

// app put <размер> <sha256> | app put b64 <…> | app put end — блок для вызова «… @»
static void put_line(char *args, app_reply_fn reply) {
  char *a1 = strtok(args, " "), *a2 = strtok(NULL, " ");
  if (a1 && !strcmp(a1, "b64")) {
    static uint8_t buf[2048];
    long n = unb64(a2, buf, sizeof buf);
    if (!g_blob || g_blob_ready || n <= 0 || g_blob_got + n > g_blob_len) {
      blob_reset();
      reply("err bad chunk");
    } else {
      memcpy(g_blob + g_blob_got, buf, n);
      g_blob_got += n;
      reply("ok");
    }
  } else if (a1 && !strcmp(a1, "end")) {
    uint8_t sha[32];
    if (g_blob && g_blob_got == g_blob_len) sha256_Raw(g_blob, g_blob_len, sha);
    if (!g_blob || g_blob_got != g_blob_len || memcmp(sha, g_blob_sha, 32)) {
      blob_reset();
      reply("err data incomplete or sha256 mismatch");
    } else {
      g_blob_ready = true;
      reply("ok");
    }
  } else {
    char *e;
    unsigned long size = a1 ? strtoul(a1, &e, 10) : 0;
    blob_reset();
    if (!size || *e || size > BLOB_MAX || !a2 || strlen(a2) != 64 || unhex_n(a2, g_blob_sha, 32) != 32) {
      reply("err usage: app put <size up to 524288> <sha256>");
    } else if (!(g_blob = malloc(size))) {
      reply("err out of memory");
    } else {
      g_blob_len = size;
      reply("ok");
    }
  }
}

// app get <смещение> — кусок длинного ответа (до 2048 байт, base64)
static void get_line(const char *arg, app_reply_fn reply) {
  static char out[8 + 2732];
  char *e;
  unsigned long off = strtoul(arg, &e, 10);
  if (!g_big || *e || off > g_big_len) {
    reply("err no such data");
    return;
  }
  uint32_t n = g_big_len - off < 2048 ? g_big_len - off : 2048;
  memcpy(out, "ok ", 3);
  b64enc((const uint8_t *)g_big + off, n, out + 3);
  reply(out);
}

static void add_end(app_reply_fn reply) {
  static slot_hdr_t h;
  uint8_t sha[32];
  mwapp_t a;
  const char *err = "no manifest";
  size_t mlen = 0;
  if (!g_inst || g_inst_got != g_inst_len) {
    reply("err incomplete app");
    inst_reset();
    return;
  }
  sha256_Raw(g_inst, g_inst_len, sha);
  const char *m = find_manifest(g_inst, g_inst_len, &mlen);
  if (memcmp(sha, g_inst_sha, 32)) err = "sha256 mismatch";
  if (memcmp(sha, g_inst_sha, 32) || !m || mlen >= sizeof h.manifest || !parse_manifest(m, mlen, &a, &err)) {
    char e[64];
    snprintf(e, sizeof e, "err %s", err);
    reply(e);
    inst_reset();
    return;
  }
  if (app_battery_low()) {
    reply("err battery low, connect USB");
    inst_reset();
    menu_draw();
    return;
  }
  const mwapp_t *old = find(a.id);
  long at = free_space(span_of(g_inst_len));
  if (at < 0) {
    reply(hal_apps_size() ? "err not enough space for apps" : "err no apps partition (flash partition table)");
    inst_reset();
    return;
  }
  char hash[65], paths[64], ver[32];
  for (int i = 0; i < 32; i++) sprintf(hash + 2 * i, "%02x", sha[i]);
  apps_paths_str(&a, paths, sizeof paths);
  if (old) snprintf(ver, sizeof ver, "%s -> %s", old->version, a.version);
  else snprintf(ver, sizeof ver, "%s", a.version);
  const ux_step_t st[] = {
      {UX_TITLE, a.icon, old ? "Update app" : "Install app", a.name},
      {UX_FIELD, ICON_INFO, "Version", ver},
      {UX_FIELD, ICON_KEY, "Key paths", paths},
      {UX_FIELD, ICON_KEY, "SHA-256", hash},
      {UX_ACCEPT, ICON_CHECK, old ? "Update" : "Install", "hold to confirm"},
      {UX_REJECT, ICON_CROSS, "Reject", "hold to cancel"},
  };
  if (!ux_review(st, 6)) {
    reply("err rejected");
    inst_reset();
    menu_draw();
    return;
  }
  // Новая копия пишется в свободное место, и только потом стирается старая: если питание
  // пропадёт посередине, при старте останется одна целая копия (apps_init)
  uint32_t span = span_of(g_inst_len);
  memset(&h, 0, sizeof h);
  memcpy(h.magic, MAGIC, sizeof MAGIC);
  memcpy(h.sha, sha, 32);
  h.len = g_inst_len;
  h.seq = old ? old->seq + 1 : 1;
  memcpy(h.manifest, m, mlen);
  // Заголовок — последним: пока его нет, недописанный модуль не считается приложением
  bool ok = hal_apps_erase((uint32_t)at, span) && hal_apps_write((uint32_t)at + HDR_SIZE, g_inst, g_inst_len) &&
            hal_apps_write((uint32_t)at, &h, sizeof h);
  if (ok && old) hal_apps_erase(old->off, old->span);
  inst_reset();
  apps_init();
  menu_reset();
  if (ok) ux_flash(ICON_CHECK, old ? "Updated" : "Installed", a.name, 900);
  menu_draw();
  reply(ok ? "ok" : "err flash write failed");
}

static bool remove_app(const mwapp_t *a) {
  const ux_step_t st[] = {
      {UX_TITLE, a->icon, "Remove app", a->name},
      {UX_ACCEPT, ICON_TRASH, "Remove", "keys stay in seed"},
      {UX_REJECT, ICON_CROSS, "Keep", "hold to cancel"},
  };
  if (!ux_review(st, 3)) return false;
  hal_apps_erase(a->off, a->span);
  apps_init();
  menu_reset();
  return true;
}

bool apps_remove(const mwapp_t *a) {
  bool ok = remove_app(a);
  menu_draw();
  return ok;
}

bool apps_line(char *line, app_reply_fn reply) {
  if (!strcmp(line, "apps")) {
    static char out[APP_LINE_MAX];
    int o = snprintf(out, sizeof out, "ok");
    for (int i = 0; i < g_count && o < (int)sizeof out - 40; i++)
      o += snprintf(out + o, sizeof out - o, "%s%s|%s|%s", i ? ";" : " ", g_apps[i].id, g_apps[i].version,
                    g_apps[i].name);
    reply(out);
    return true;
  }
  if (strncmp(line, "app ", 4)) return false;
  char *rest = line + 4;
  if (!strncmp(rest, "add ", 4)) {
    char *a1 = strtok(rest + 4, " "), *a2 = strtok(NULL, " ");
    if (a1 && !strcmp(a1, "b64")) {
      uint8_t buf[2048];
      long n = unb64(a2, buf, sizeof buf);
      if (!g_inst || n <= 0 || g_inst_got + n > g_inst_len) {
        inst_reset();
        reply("err bad chunk");
      } else {
        memcpy(g_inst + g_inst_got, buf, n);
        g_inst_got += n;
        reply("ok");
      }
    } else if (a1 && !strcmp(a1, "end")) {
      add_end(reply);
    } else {
      char *e;
      unsigned long size = a1 ? strtoul(a1, &e, 10) : 0;
      inst_reset();
      if (!size || *e || size > APP_MAX || !a2 || strlen(a2) != 64 || unhex_n(a2, g_inst_sha, 32) != 32) {
        reply("err usage: app add <size up to 65024> <sha256>");
      } else if (!(g_inst = malloc(size))) {
        reply("err out of memory");
      } else {
        g_inst_len = size;
        reply("ok");
      }
    }
    return true;
  }
  if (!strncmp(rest, "put ", 4)) {
    put_line(rest + 4, reply);
    return true;
  }
  if (!strncmp(rest, "get ", 4)) {
    get_line(rest + 4, reply);
    return true;
  }
  if (!wallet_is_unlocked()) {
    reply("err no wallet on device");
    return true;
  }
  if (!strncmp(rest, "rm ", 3)) {
    const mwapp_t *a = find(rest + 3);
    if (!a) reply("err no such app");
    else reply(apps_remove(a) ? "ok" : "err rejected");
    return true;
  }
  char *sp = strchr(rest, ' ');
  if (sp) *sp = 0;
  const mwapp_t *a = find(rest);
  if (!a) {
    reply("err no such app");
    return true;
  }
  // « @» в конце — вызову отдаётся загруженный блок; он одноразовый
  const char *cmd = sp ? sp + 1 : "";
  size_t cl = strlen(cmd);
  bool at = !strcmp(cmd, "@") || (cl >= 2 && !strcmp(cmd + cl - 2, " @"));
  if (at && !g_blob_ready) {
    reply("err no data: send it with app put first");
    return true;
  }
  big_reset();  // прошлый длинный ответ больше не нужен
  g_blob_on = at;
  run(a, cmd, reply);
  g_blob_on = false;
  if (at) blob_reset();
  return true;
}
