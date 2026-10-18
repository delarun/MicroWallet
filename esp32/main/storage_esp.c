// storage_esp.c — seed, настройки и BLE-сопряжения в NVS.
// В обычной сборке NVS не зашифрован. В защищённой
// (sdkconfig.secure) включены шифрование флеша и NVS — ключи в eFuse.
#include <string.h>

#include "channel.h"
#include "hal.h"
#include "memzero.h"
#include "nvs.h"
#include "storage.h"

#define NS "pw"
#define BONDS 4

typedef struct {
  uint8_t used, id[8], key[32];
} bond_t;

static bool get_blob(const char *key, void *buf, size_t *len) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = nvs_get_blob(h, key, buf, len) == ESP_OK;
  nvs_close(h);
  return ok;
}

static bool set_blob(const char *key, const void *buf, size_t len) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

static void erase(const char *key) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

bool storage_load(uint8_t entropy[32], size_t *len) {
  size_t n = 32;
  if (!get_blob("seed", entropy, &n) || (n != 16 && n != 24 && n != 32)) return false;
  *len = n;
  return true;
}

bool storage_save(const uint8_t *entropy, size_t len) {
  if (len != 16 && len != 24 && len != 32) return false;
  return set_blob("seed", entropy, len);
}

void storage_wipe(void) {
  erase("seed");
  erase("pin");
}

bool storage_pin_load(pin_rec_t *r) {
  size_t n = sizeof *r;
  return get_blob("pin", r, &n) && n == sizeof *r && r->ver == 1;
}
bool storage_pin_save(const pin_rec_t *r) { return set_blob("pin", r, sizeof *r); }
void storage_pin_clear(void) { erase("pin"); }

void settings_load(settings_t *s) {
  settings_t t;
  size_t n = sizeof t;
  *s = SETTINGS_DEFAULT;
  if (get_blob("settings", &t, &n) && (n == sizeof t || n == SETTINGS_V1_SIZE)) memcpy(s, &t, n);
}

bool settings_save(const settings_t *s) { return set_blob("settings", s, sizeof *s); }

void apps_order_load(char *out, size_t len) {
  size_t n = len - 1;
  if (!get_blob("order", out, &n)) n = 0;
  out[n] = 0;
}

bool apps_order_save(const char *s) { return set_blob("order", s, strlen(s)); }

// ---------- сопряжения BLE: до 4 ПК ----------

static void bonds_load(bond_t b[BONDS]) {
  size_t n = sizeof(bond_t) * BONDS;
  if (!get_blob("bonds", b, &n) || n != sizeof(bond_t) * BONDS) memset(b, 0, sizeof(bond_t) * BONDS);
}

bool bond_find(const uint8_t id[8], uint8_t key[32]) {
  bond_t b[BONDS];
  bool found = false;
  bonds_load(b);
  for (int i = 0; i < BONDS && !found; i++)
    if (b[i].used && !memcmp(b[i].id, id, 8)) {
      memcpy(key, b[i].key, 32);
      found = true;
    }
  memzero(b, sizeof b);
  return found;
}

bool bond_add(const uint8_t id[8], const uint8_t key[32]) {
  bond_t b[BONDS];
  bonds_load(b);
  int slot = -1;
  for (int i = 0; i < BONDS && slot < 0; i++)
    if (!b[i].used) slot = i;
  if (slot < 0) {  // всё занято — вытесняем самую старую запись
    memmove(b, b + 1, sizeof(bond_t) * (BONDS - 1));
    slot = BONDS - 1;
  }
  b[slot].used = 1;
  memcpy(b[slot].id, id, 8);
  memcpy(b[slot].key, key, 32);
  bool ok = set_blob("bonds", b, sizeof b);
  memzero(b, sizeof b);
  return ok;
}

void bond_forget_all(void) { erase("bonds"); }

void hal_ble_forget_all(void) { bond_forget_all(); }
