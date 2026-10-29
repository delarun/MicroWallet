#include "pin.h"

#include <string.h>

#include "button.h"
#include "hal.h"
#include "memzero.h"
#include "oled.h"
#include "sha2.h"
#include "storage.h"
#include "ui.h"

#define IDLE_TIMEOUT_US 60000000ULL  // ввод внутри подтверждения: минута без нажатий — отмена
#define DEBOUNCE_MS 30

int pin_digit(uint32_t ms) {
  uint32_t d = ms / 500;
  return d < 1 ? 1 : d > 9 ? 9 : (int)d;
}

static void pin_hash(const uint8_t salt[16], const uint8_t d[PIN_LEN], uint8_t out[32]) {
  static const char TAG[] = "MicroWallet PIN v1";
  SHA256_CTX c;
  sha256_Init(&c);
  sha256_Update(&c, (const uint8_t *)TAG, sizeof TAG);
  sha256_Update(&c, salt, 16);
  sha256_Update(&c, d, PIN_LEN);
  sha256_Final(&c, out);
  memzero(&c, sizeof c);
}

bool pin_is_set(void) {
  pin_rec_t r;
  return storage_pin_load(&r);
}

int pin_fails(void) {
  pin_rec_t r;
  return storage_pin_load(&r) ? r.fails : 0;
}

uint32_t pin_delay_s(void) {
  int f = pin_fails();
#ifdef MW_SIM
  extern bool sim_no_delay(void);
  if (sim_no_delay()) return 0;
#endif
  return f < 3 ? 0 : 5u << (f - 3);  // 5, 10, 20 … 320 с
}

bool pin_set(const uint8_t d[PIN_LEN]) {
  pin_rec_t r = {.ver = 1, .fails = 0};
  hal_random(r.salt, sizeof r.salt);
  pin_hash(r.salt, d, r.hash);
  bool ok = storage_pin_save(&r);
  memzero(&r, sizeof r);
  return ok;
}

void pin_clear(void) { storage_pin_clear(); }

pin_res_t pin_check(const uint8_t d[PIN_LEN], int *left) {
  pin_rec_t r;
  uint8_t h[32];
  if (!storage_pin_load(&r)) return PIN_OK;
  // Сначала счётчик — во флеш: сбой питания посреди проверки не даст лишней попытки
  r.fails++;
  if (!storage_pin_save(&r)) {
    *left = 0;
    return PIN_WRONG;
  }
  pin_hash(r.salt, d, h);
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ r.hash[i]);
  memzero(h, sizeof h);
  if (!diff) {
    r.fails = 0;
    storage_pin_save(&r);
    return PIN_OK;
  }
  if (r.fails >= PIN_MAX_FAILS) {
    storage_wipe();  // seed и PIN
    return PIN_WIPED;
  }
  *left = PIN_MAX_FAILS - r.fails;
  return PIN_WRONG;
}

// ---------------- ввод ----------------

void pin_entry_start(pin_entry_t *e, const char *title) {
  memset(e, 0, sizeof *e);
  e->title = title;
  e->level = -1;
  e->wait_release = btn_physical();  // удержание «Accept» ещё не отпущено
  e->dirty = true;
  e->t_idle = hal_time_us();
}

bool pin_entry_step(pin_entry_t *e) {
#ifdef MW_SIM
  if (sim_pin_typed(e->d)) return true;
#endif
  bool p = btn_physical();  // цифры — только кнопкой
  uint64_t now = hal_time_us();
  if (e->wait_release) {
    if (!p) e->wait_release = false;
    p = false;
  } else if (p && !e->down) {
    e->down = true;
    e->t_down = now;
    e->level = e->confirm ? -1 : 1;  // нажатие уже стоит 1
    e->dirty = true;
  } else if (p && e->down && !e->confirm) {
    int lvl = pin_digit((uint32_t)((now - e->t_down) / 1000));
    if (lvl != e->level) {
      e->level = lvl;
      e->dirty = true;
    }
  } else if (!p && e->down) {
    uint32_t ms = (uint32_t)((now - e->t_down) / 1000);
    e->down = false;
    e->level = -1;
    e->dirty = true;
    e->t_idle = now;
    if (ms >= DEBOUNCE_MS) {
      if (e->confirm) {
        if (ms < BTN_HOLD_MS) return true;  // клик — ввести
        e->confirm = false;                 // удержание — заново
        e->n = 0;
        memzero(e->d, sizeof e->d);
      } else {
        e->d[e->n++] = (uint8_t)pin_digit(ms);
        if (e->n == PIN_LEN) e->confirm = true;
      }
    }
  }
  if (e->dirty) {
    ui_pin(e->title, e->n, e->level, e->confirm);
    oled_show();
    e->dirty = false;
  }
  return false;
}

bool pin_prompt(const char *title, uint8_t d[PIN_LEN]) {
  pin_entry_t e;
  pin_entry_start(&e, title);
#ifdef MW_SIM
  if (!sim_pin_typed(e.d)) return false;  // никто ничего не набрал — как отмена по таймауту
  memcpy(d, e.d, PIN_LEN);
  memzero(&e, sizeof e);
  return true;
#endif
  for (;;) {
    if (pin_entry_step(&e)) break;
    if (!e.down && hal_time_us() - e.t_idle > IDLE_TIMEOUT_US) {
      memzero(&e, sizeof e);
      return false;
    }
    hal_sleep_ms(10);
  }
  memcpy(d, e.d, PIN_LEN);
  memzero(&e, sizeof e);
  return true;
}
