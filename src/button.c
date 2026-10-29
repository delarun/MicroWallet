#include "button.h"

#include <stdlib.h>

#include "hal.h"
#include "oled.h"

// Координаты касания — тысячные доли экрана (hal_touch). Экран Waveshare 33x18 мм:
#define SWIPE_X 70   // ~2.3 мм по горизонтали
#define SWIPE_Y 130  // ~2.3 мм по вертикали; короче — касание, а не свайп
#define ASPECT_X 33  // ширина и высота экрана — сравнивать сдвиг в миллиметрах
#define ASPECT_Y 18
#define EDGE 250     // касание у края (четверть ширины) — листать, посередине — выбрать

// Какой пин и как подключена кнопка — решает hal_btn_raw() платформы
void btn_init(void) { hal_btn_init(); }

bool btn_physical(void) { return hal_btn_raw(); }

bool btn_pressed(void) {
  int x, y;
  return hal_btn_raw() || hal_touch(&x, &y);
}

static bool down, fired;
static uint64_t t_down;
static uint32_t last_us;

// Палец на экране: где коснулся, где сейчас, сдвинулся ли (тогда это свайп, не удержание)
static bool t_on, t_fired, t_moved, was_swipe;
static int tx0, ty0, tx, ty;
static uint64_t t_touch;
// Скорость: точка не моложе VEL_US назад и посчитанная по ней скорость
#define VEL_US 40000
static int vx_ref, vx;
static uint64_t vt_ref, vt;

// Удержание (кнопки или пальца на месте): полоса внизу, через BTN_HOLD_MS — событие
static bool hold_tick(uint64_t now, uint64_t since, bool *done) {
  uint32_t ms = (uint32_t)((now - since) / 1000);
  if (!*done && ms >= 250) oled_hold_progress(ms * 100 / BTN_HOLD_MS);
  if (!*done && ms >= BTN_HOLD_MS) {
    *done = true;
    last_us = (uint32_t)(now - since);
    oled_hold_progress(-1);
    return true;
  }
  return false;
}

static btn_t touch_poll(uint64_t now) {
  int x, y;
  bool p = hal_touch(&x, &y);
  if (p && !t_on) {
    t_on = true;
    t_fired = t_moved = false;
    tx0 = tx = vx_ref = x;
    ty0 = ty = y;
    t_touch = vt_ref = now;
    vx = 0;
    return BTN_NONE;
  }
  if (p && t_on) {
    tx = x;
    ty = y;
    if (now - vt_ref >= VEL_US) {
      vx = (int)((int64_t)(x - vx_ref) * 1000000 / (int64_t)(now - vt_ref));
      vx_ref = x;
      vt_ref = vt = now;
    }
    if (!t_moved && (abs(tx - tx0) >= SWIPE_X || abs(ty - ty0) >= SWIPE_Y)) {
      t_moved = true;
      if (!t_fired) oled_hold_progress(-1);
    }
    if (!t_moved && hold_tick(now, t_touch, &t_fired)) return BTN_HOLD;
    return BTN_NONE;
  }
  if (!p && t_on) {
    t_on = false;
    if (t_fired) return BTN_NONE;
    oled_hold_progress(-1);
    last_us = (uint32_t)(now - t_touch);
    was_swipe = t_moved;
    if (vt < t_touch)  // короткий взмах: замеров скорости не было — по всему пути
      vx = now > t_touch ? (int)((int64_t)(tx - tx0) * 1000000 / (int64_t)(now - t_touch)) : 0;
    else if (now - vt > 120000)
      vx = 0;  // палец остановился перед тем, как его отпустили
    if (t_moved) {
      int dx = tx - tx0, dy = ty - ty0;
      if (abs(dx) * ASPECT_X >= abs(dy) * ASPECT_Y) return dx < 0 ? BTN_TAP : BTN_PREV;  // как страницы
      return dy < 0 ? BTN_TAP : BTN_PREV;
    }
    if (tx0 < EDGE) return BTN_PREV;
    if (tx0 >= 1000 - EDGE) return BTN_TAP;
    return BTN_SELECT;
  }
  return BTN_NONE;
}

bool btn_drag(int *dx, int *dy) {
  if (!t_on || !t_moved || t_fired) return false;
  *dx = tx - tx0;
  *dy = ty - ty0;
  return true;
}

bool btn_was_swipe(void) { return was_swipe; }
int btn_swipe_vx(void) { return vx; }

btn_t btn_poll(void) {
  bool p = hal_btn_raw();
  uint64_t now = hal_time_us();
  if (p && !down) {
    down = true;
    fired = false;
    t_down = now;
    return BTN_NONE;
  }
  if (p && down) return hold_tick(now, t_down, &fired) ? BTN_HOLD : BTN_NONE;
  if (!p && down) {
    down = false;
    was_swipe = false;
    uint32_t us = (uint32_t)(now - t_down);
    if (fired) return BTN_NONE;
    oled_hold_progress(-1);
    if (us < 30000) return BTN_NONE;  // дребезг
    last_us = us;
    return BTN_TAP;
  }
  return touch_poll(now);
}

btn_t btn_wait(void) {
  for (;;) {
    btn_t b = btn_poll();
    if (b != BTN_NONE) return b;
    hal_sleep_ms(10);
  }
}

uint32_t btn_last_press_us(void) { return last_us; }
