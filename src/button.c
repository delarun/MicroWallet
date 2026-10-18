#include "button.h"

#include "hal.h"
#include "oled.h"

// Какой пин и как подключена кнопка — решает hal_btn_raw() платформы
void btn_init(void) { hal_btn_init(); }

bool btn_pressed(void) { return hal_btn_raw(); }

static bool down, fired;
static uint64_t t_down;
static uint32_t last_us;

btn_t btn_poll(void) {
  bool p = btn_pressed();
  uint64_t now = hal_time_us();
  if (p && !down) {
    down = true;
    fired = false;
    t_down = now;
    return BTN_NONE;
  }
  if (p && down) {
    uint32_t ms = (uint32_t)((now - t_down) / 1000);
    if (!fired && ms >= 250) oled_hold_progress(ms * 100 / BTN_HOLD_MS);
    if (!fired && ms >= BTN_HOLD_MS) {
      fired = true;
      last_us = (uint32_t)(now - t_down);
      oled_hold_progress(-1);
      return BTN_HOLD;
    }
    return BTN_NONE;
  }
  if (!p && down) {
    down = false;
    uint32_t us = (uint32_t)(now - t_down);
    if (fired) return BTN_NONE;
    oled_hold_progress(-1);
    if (us < 30000) return BTN_NONE;  // дребезг
    last_us = us;
    return BTN_TAP;
  }
  return BTN_NONE;
}

btn_t btn_wait(void) {
  for (;;) {
    btn_t b = btn_poll();
    if (b != BTN_NONE) return b;
    hal_sleep_ms(10);
  }
}

uint32_t btn_last_press_us(void) { return last_us; }
