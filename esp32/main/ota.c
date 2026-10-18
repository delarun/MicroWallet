#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "hal.h"
#include "icons.h"
#include "memzero.h"
#include "miniz.h"  // esp_rom: tinfl_decompress лежит в ROM ESP32-S3
#include "menu.h"
#include "oled.h"
#include "sha2.h"
#include "ui.h"
#include "ux.h"

// base64 от 2048 байт — 2732 символа: вместе с "ota b64 " строка укладывается в
// APP_LINE_MAX (3072), а кадр канала — в CHAN_FRAME_MAX
#define OTA_CHUNK 2048
#define OTA_IDLE_US (30 * 1000000ULL)  // данных нет 30 с — передача оборвалась

static esp_ota_handle_t h;
static const esp_partition_t *part;
static size_t total, done;
static SHA256_CTX sha;
static uint8_t want[32];
static bool active, over_ble;
static int shown = -1;
static uint64_t last_us;

// Сжатый поток (ota zbegin): zlib, распаковывается на лету в запись OTA. Образ жмётся
// примерно на 40% — по BLE это и есть 40% времени. Размер и SHA-256 — распакованного образа.
static tinfl_decompressor *zr;
static uint8_t *zdict;  // окно словаря 32 КБ (кольцевое)
static size_t zpos;
static bool zmode, zdone;

static long unhex(const char *s, uint8_t *out, size_t max) {
  size_t n = s ? strlen(s) : 0;
  if (n % 2 || n / 2 > max) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    unsigned v;
    if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return (long)(n / 2);
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

// Пока идёт обновление — полная частота и никакого лёгкого сна: во сне USB Serial/JTAG
// стоит, и строки с данными терялись (особенно с шифрованием флеша — запись дольше
// держит прерывания выключенными, и проверка «USB на связи» ошибалась).
static void pm_hold(bool on) {
#if CONFIG_PM_ENABLE
  static esp_pm_lock_handle_t lock;
  static bool held;
  if (!lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ota", &lock) != ESP_OK) return;
  if (on == held) return;
  if (on) esp_pm_lock_acquire(lock);
  else esp_pm_lock_release(lock);
  held = on;
#endif
}

static void z_free(void) {
  free(zr);
  free(zdict);
  zr = NULL;
  zdict = NULL;
  zmode = zdone = false;
}

static void stop(void) {
  if (active) esp_ota_abort(h);
  z_free();
  active = false;
  shown = -1;
  pm_hold(false);
  app_busy(false);
}

static void progress(void) {
  int pct = total ? (int)(done * 100 / total) : 0;
  if (pct == shown) return;
  shown = pct;
  ui_progress(ICON_SEND, "Updating", pct);
  oled_show();
}

static void begin(const char *size_s, const char *sha_s, bool z, app_reply_fn reply) {
  char *e;
  unsigned long size = size_s ? strtoul(size_s, &e, 10) : 0;
  if (!size || *e || unhex(sha_s, want, 32) != 32) {
    reply("err usage: ota begin|zbegin <size> <sha256>");
    return;
  }
  stop();
  if (z) {
    zr = malloc(sizeof *zr);
    zdict = malloc(TINFL_LZ_DICT_SIZE);
    if (!zr || !zdict) {
      z_free();
      reply("err out of memory");
      return;
    }
    tinfl_init(zr);
    zpos = 0;
    zmode = true;
  }
  if (app_battery_low()) {  // запись прошивки — долгий пик тока
    reply("err battery low, connect USB");
    menu_draw();
    return;
  }
  part = esp_ota_get_next_update_partition(NULL);
  if (!part || size > part->size) {
    reply("err image too big");
    return;
  }
  char sz[24], hash[65];
  snprintf(sz, sizeof sz, "%lu bytes", size);
  for (int i = 0; i < 32; i++) sprintf(hash + 2 * i, "%02x", want[i]);
  const ux_step_t st[] = {
      {UX_TITLE, ICON_SEND, "Update firmware", over_ble ? "over Bluetooth" : "from PC over USB"},
      {UX_FIELD, ICON_INFO, "Size", sz},
      {UX_FIELD, ICON_KEY, "SHA-256", hash},
      {UX_ACCEPT, ICON_CHECK, "Install", "hold to update"},
      {UX_REJECT, ICON_CROSS, "Reject", "hold to cancel"},
  };
  if (!ux_review(st, 5)) {
    z_free();
    reply("err rejected");
    ux_flash(ICON_CROSS, "Rejected", NULL, 900);
    menu_draw();
    return;
  }
  if (esp_ota_begin(part, size, &h) != ESP_OK) {
    z_free();
    reply("err ota begin failed");
    menu_draw();
    return;
  }
  total = size;
  done = 0;
  active = true;
  pm_hold(true);
  last_us = hal_time_us();
  app_busy(true);  // экран и кнопка — только у обновления, меню не перерисовывается
  sha256_Init(&sha);
  progress();
  reply("ok");
}

// Готовые байты образа: в слот OTA и в SHA-256. 0 — ок, иначе текст ошибки
static const char *sink(const uint8_t *p, size_t n) {
  if (done + n > total) return "err bad chunk";
  if (esp_ota_write(h, p, n) != ESP_OK) return "err flash write failed";
  sha256_Update(&sha, p, n);
  done += n;
  return 0;
}

// Кусок сжатого потока -> распаковка в окно словаря -> sink
static const char *z_feed(const uint8_t *in, size_t n) {
  for (;;) {
    size_t in_n = n, out_n = TINFL_LZ_DICT_SIZE - zpos;
    tinfl_status st = tinfl_decompress(zr, in, &in_n, zdict, zdict + zpos, &out_n,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
    in += in_n;
    n -= in_n;
    if (out_n) {
      const char *e = sink(zdict + zpos, out_n);
      if (e) return e;
      zpos = (zpos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (st == TINFL_STATUS_DONE) {
      zdone = true;
      return n ? "err bad chunk" : 0;  // после конца потока данных быть не должно
    }
    if (st < 0) return "err bad compressed data";
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !n) return 0;
    if (zdone) return "err bad chunk";
  }
}

static void data(const char *enc, bool b64, app_reply_fn reply) {
  static uint8_t buf[OTA_CHUNK];
  long n = b64 ? unb64(enc, buf, sizeof buf) : unhex(enc, buf, sizeof buf);
  const char *e = 0;
  if (!active) {
    reply("err no update in progress");
    return;
  }
  if (n <= 0) e = "err bad chunk";
  else if (zmode) e = zdone ? "err bad chunk" : z_feed(buf, (size_t)n);
  else e = sink(buf, (size_t)n);
  if (e) {
    stop();
    reply(e);
    menu_draw();
    return;
  }
  last_us = hal_time_us();
  progress();
  reply("ok");
}

static void end(app_reply_fn reply) {
  uint8_t got[32];
  if (!active || done != total || (zmode && !zdone)) {
    stop();
    reply("err incomplete image");
    menu_draw();
    return;
  }
  sha256_Final(&sha, got);
  if (memcmp(got, want, 32)) {
    stop();
    reply("err sha256 mismatch");
    ux_error("sha256 mismatch");
    menu_draw();
    return;
  }
  active = false;
  z_free();
  // esp_ota_end проверяет образ, а при Secure Boot — и его подпись
  esp_err_t err = esp_ota_end(h);
  pm_hold(false);
  if (err != ESP_OK) {
    app_busy(false);
    reply(err == ESP_ERR_OTA_VALIDATE_FAILED ? "err image invalid or not signed" : "err ota end failed");
    ux_error("image rejected");
    menu_draw();
    return;
  }
  if (esp_ota_set_boot_partition(part) != ESP_OK) {
    app_busy(false);
    reply("err cannot switch partition");
    menu_draw();
    return;
  }
  reply("ok rebooting");
  ux_flash(ICON_CHECK, "Updated", "restarting", 1200);
  esp_restart();
}

static void aborted(const char *why) {
  stop();
  ux_flash(ICON_CROSS, "Update aborted", why, 1500);
  menu_draw();
}

void ota_tick(void) {
  if (active && hal_time_us() - last_us > OTA_IDLE_US) aborted("no data");
}

void ota_link_lost(void) {
  if (active && over_ble) aborted("link lost");
}

bool ota_line(char *line, app_reply_fn reply, bool ble) {
  if (strncmp(line, "ota ", 4)) return false;
  char *cmd = strtok(line + 4, " "), *a1 = strtok(NULL, " "), *a2 = strtok(NULL, " ");
  if (cmd && (!strcmp(cmd, "begin") || !strcmp(cmd, "zbegin"))) over_ble = ble;
  if (!cmd) reply("err usage");
  else if (!strcmp(cmd, "begin")) begin(a1, a2, false, reply);
  else if (!strcmp(cmd, "zbegin")) begin(a1, a2, true, reply);
  else if (!strcmp(cmd, "b64")) data(a1, true, reply);
  else if (!strcmp(cmd, "data")) data(a1, false, reply);
  else if (!strcmp(cmd, "end")) end(reply);
  else if (!strcmp(cmd, "abort")) {
    stop();
    reply("ok");
    menu_draw();
  } else reply("err unknown ota command");
  return true;
}
