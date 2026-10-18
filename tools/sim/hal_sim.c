// hal_sim.c — «плата» для симулятора на ПК: файлы вместо флеша, без экрана.
// Каталог хранилища — $MW_SIM_DIR (по умолчанию текущий).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "channel.h"
#include "hal.h"
#include "storage.h"

static char path_buf[512];
static const char *path(const char *name) {
  const char *d = getenv("MW_SIM_DIR");
  snprintf(path_buf, sizeof path_buf, "%s/%s", d ? d : ".", name);
  return path_buf;
}

uint64_t hal_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
void hal_sleep_ms(uint32_t ms) { (void)ms; }  // симулятор не ждёт
void hal_random(uint8_t *buf, size_t len) {
  if (getrandom(buf, len, 0) != (ssize_t)len) abort();
}
void hal_entropy(uint8_t out[32]) { hal_random(out, 32); }
void hal_btn_init(void) {}
bool hal_btn_raw(void) { return false; }
void hal_i2c_init(void) {}
bool hal_i2c_write(const uint8_t *buf, size_t len) { (void)buf; (void)len; return true; }
void hal_reboot_bootloader(void) {}
const char *hal_platform(void) { return "sim"; }
bool hal_secure(void) { return false; }
const char *hal_ota_info(void) { return "sim"; }
const char *hal_fw_build(void) { return "-"; }
bool hal_usb_connected(void) { return true; }
bool hal_ble_available(void) { return true; }
bool hal_ble_enabled(void) { return true; }
bool hal_ble_connected(void) { return false; }
void hal_ble_set_enabled(bool on) { (void)on; }
void hal_ble_forget_all(void) { bond_forget_all(); }
int hal_battery_mv(void) { return -1; }
bool hal_charging(void) { return false; }
void hal_power_save(bool on) { (void)on; }
bool hal_power_off_available(void) { return false; }
void hal_power_off(void) {}
bool hal_resumed(void) { return false; }
const char *hal_boot_reason(void) { return "sim"; }
void hal_heap(uint32_t *f, uint32_t *l) { *f = *l = 0; }

// Ответ на экран подтверждения: $MW_SIM_ANSWER=reject — отказ
bool sim_answer(void) {
  const char *a = getenv("MW_SIM_ANSWER");
  return !(a && !strcmp(a, "reject"));
}

// ---------- хранилище ----------

static bool read_file(const char *name, void *buf, size_t len) {
  FILE *f = fopen(path(name), "rb");
  if (!f) return false;
  size_t n = fread(buf, 1, len, f);
  fclose(f);
  return n == len;
}

static bool write_file(const char *name, const void *buf, size_t len) {
  FILE *f = fopen(path(name), "wb");
  if (!f) return false;
  bool ok = fwrite(buf, 1, len, f) == len;
  fclose(f);
  return ok;
}

bool storage_load(uint8_t entropy[32], size_t *len) {
  uint8_t b[33];
  if (!read_file("seed.bin", b, sizeof b) || (b[0] != 16 && b[0] != 24 && b[0] != 32)) return false;
  *len = b[0];
  memcpy(entropy, b + 1, b[0]);
  return true;
}

bool storage_save(const uint8_t *entropy, size_t len) {
  uint8_t b[33] = {(uint8_t)len};
  memcpy(b + 1, entropy, len);
  return write_file("seed.bin", b, sizeof b);
}

void storage_wipe(void) {
  remove(path("seed.bin"));
  remove(path("pin.bin"));
}

bool storage_pin_load(pin_rec_t *r) { return read_file("pin.bin", r, sizeof *r) && r->ver == 1; }
bool storage_pin_save(const pin_rec_t *r) { return write_file("pin.bin", r, sizeof *r); }
void storage_pin_clear(void) { remove(path("pin.bin")); }

// ---------- PIN: что «набрал человек» ----------

static char g_typed[8];
static bool g_typed_init;

void sim_pin_type(const char *digits) {
  snprintf(g_typed, sizeof g_typed, "%s", digits ? digits : "");
  g_typed_init = true;
}

bool sim_pin_typed(uint8_t d[4]) {
  if (!g_typed_init) sim_pin_type(getenv("MW_SIM_PIN"));
  if (strlen(g_typed) != 4) return false;
  for (int i = 0; i < 4; i++) {
    if (g_typed[i] < '1' || g_typed[i] > '9') return false;
    d[i] = (uint8_t)(g_typed[i] - '0');
  }
  return true;
}

bool sim_no_delay(void) { return getenv("MW_SIM_NODELAY") != NULL; }

void settings_load(settings_t *s) {
  settings_t t = SETTINGS_DEFAULT;
  if (read_file("settings.bin", &t, sizeof t) || read_file("settings.bin", &t, SETTINGS_V1_SIZE)) *s = t;
  else *s = SETTINGS_DEFAULT;
}
bool settings_save(const settings_t *s) { return write_file("settings.bin", s, sizeof *s); }

void apps_order_load(char *out, size_t len) {
  FILE *f = fopen(path("order.txt"), "rb");
  size_t n = f ? fread(out, 1, len - 1, f) : 0;
  if (f) fclose(f);
  out[n] = 0;
}

bool apps_order_save(const char *s) { return write_file("order.txt", s, strlen(s)); }

// ---------- сопряжения: до 4 записей id[8] + key[32] ----------

#define BONDS 4
typedef struct {
  uint8_t used, id[8], key[32];
} bond_t;

static void bonds_load(bond_t b[BONDS]) {
  if (!read_file("bonds.bin", b, sizeof(bond_t) * BONDS)) memset(b, 0, sizeof(bond_t) * BONDS);
}

bool bond_find(const uint8_t id[8], uint8_t key[32]) {
  bond_t b[BONDS];
  bonds_load(b);
  for (int i = 0; i < BONDS; i++)
    if (b[i].used && !memcmp(b[i].id, id, 8)) {
      memcpy(key, b[i].key, 32);
      return true;
    }
  return false;
}

bool bond_add(const uint8_t id[8], const uint8_t key[32]) {
  bond_t b[BONDS];
  bonds_load(b);
  int slot = 0;
  for (int i = 0; i < BONDS; i++)
    if (!b[i].used) {
      slot = i;
      break;
    }
  b[slot].used = 1;  // если всё занято — вытесняем первую запись
  memcpy(b[slot].id, id, 8);
  memcpy(b[slot].key, key, 32);
  return write_file("bonds.bin", b, sizeof b);
}

void bond_forget_all(void) { remove(path("bonds.bin")); }

// ---------- приложения: файл apps.bin (1.5 МБ, как раздел на плате) ----------

#define SIM_APPS_SIZE 0x180000

uint32_t hal_apps_size(void) { return SIM_APPS_SIZE; }

static FILE *apps_file(void) {
  FILE *f = fopen(path("apps.bin"), "r+b");
  if (!f) {  // как стёртый флеш
    f = fopen(path("apps.bin"), "w+b");
    if (!f) return NULL;
    static uint8_t ff[HAL_APPS_SECTOR];
    memset(ff, 0xFF, sizeof ff);
    for (uint32_t o = 0; o < SIM_APPS_SIZE; o += sizeof ff) fwrite(ff, 1, sizeof ff, f);
  }
  return f;
}

bool hal_apps_read(uint32_t off, void *buf, uint32_t len) {
  FILE *f = apps_file();
  if (!f || off + len > SIM_APPS_SIZE) return false;
  bool ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
  fclose(f);
  return ok;
}

bool hal_apps_write(uint32_t off, const void *data, uint32_t len) {
  FILE *f = apps_file();
  if (!f || off + len > SIM_APPS_SIZE) return false;
  bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

bool hal_apps_erase(uint32_t off, uint32_t len) {
  static uint8_t ff[HAL_APPS_SECTOR];
  if (off % HAL_APPS_SECTOR || len % HAL_APPS_SECTOR || off + len > SIM_APPS_SIZE) return false;
  memset(ff, 0xFF, sizeof ff);
  for (uint32_t o = 0; o < len; o += sizeof ff)
    if (!hal_apps_write(off + o, ff, sizeof ff)) return false;
  return true;
}
