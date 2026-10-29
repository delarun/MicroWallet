#include "ux.h"

#include <string.h>

#include "button.h"
#include "icons.h"
#include "oled.h"
#include "hal.h"
#include "ui.h"

// Сумма длиннее 10 символов не влезает крупным шрифтом — показываем как поле
static bool as_field(const ux_step_t *s) {
  return s->kind == UX_FIELD || (s->kind == UX_AMOUNT && strlen(s->text) > 10);
}

static int step_pages(const ux_step_t *s) { return as_field(s) ? ui_pages(s->text) : 1; }

static void draw(const ux_step_t *steps, int n, int screen, int total) {
  for (int i = 0, first = 0; i < n; first += step_pages(&steps[i]), i++) {
    const ux_step_t *s = &steps[i];
    int pages = step_pages(s);
    if (screen >= first + pages) continue;
    if (as_field(s)) ui_field(s->icon, s->title, s->text, screen - first, pages, screen, total);
    else if (s->kind == UX_AMOUNT) ui_amount(s->icon, s->title, s->text, screen, total);
    else ui_carousel(s->icon, s->title, s->text, screen, total);
    return;
  }
}

static ux_kind_t kind_at(const ux_step_t *steps, int n, int screen) {
  for (int i = 0, first = 0; i < n; first += step_pages(&steps[i]), i++)
    if (screen < first + step_pages(&steps[i])) return steps[i].kind;
  return UX_TITLE;
}

#ifdef MW_SIM
#include <stdio.h>
bool sim_answer(void);  // tools/sim/hal_sim.c
#endif

bool ux_review(const ux_step_t *steps, int n) {
#ifdef MW_SIM
  // Симулятор: печатаем шаги в stderr и отвечаем по $MW_SIM_ANSWER
  for (int i = 0; i < n; i++)
    fprintf(stderr, "[screen] %s: %s\n", steps[i].title, steps[i].text ? steps[i].text : "");
  return sim_answer();
#endif
  int total = 0, cur = 0;
  for (int i = 0; i < n; i++) total += step_pages(&steps[i]);
  ui_anim(UI_ANIM_FADE);
  for (;;) {
    draw(steps, n, cur, total);
    oled_show();
    btn_t b = btn_wait();
    ux_kind_t k = kind_at(steps, n, cur);
    ui_anim_btn(b);
    if (b == BTN_HOLD) {
      if (k == UX_ACCEPT) return true;
      if (k == UX_REJECT) return false;
    } else if (b == BTN_SELECT && k == UX_REJECT) {
      return false;  // отказаться можно и касанием; согласиться — только удержанием
    } else if (b == BTN_PREV) {
      cur = (cur + total - 1) % total;
    } else if (b == BTN_TAP || (b == BTN_SELECT && k != UX_ACCEPT)) {
      cur = (cur + 1) % total;
    }
  }
}

void ux_flash(const uint8_t *icon, const char *title, const char *sub, int ms) {
  ui_message(icon, title, sub);
  oled_show();
  if (ms > 0) hal_sleep_ms(ms);
  else btn_wait();
}

void ux_error(const char *msg) {
  ui_field(ICON_WARN, "Error", msg, 0, 1, -1, 0);
  oled_show();
  hal_sleep_ms(2500);
}
