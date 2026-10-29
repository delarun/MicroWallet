#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "icons.h"

// Раскладка зависит от высоты экрана: 64 строки (SSD1306 128x64) или 32
#define TALL (GFX_H >= 64)
#define DOTS_Y (TALL ? GFX_H - 5 : 28)    // точки навигации
#define FIELD_LINES (TALL ? 5 : 2)        // строк значения на страницу

// Точки внизу: текущая — чёрточка, остальные — точки 2x2. Если экранов
// слишком много, точки не рисуем (номер страницы есть в заголовке).
static void dots(int idx, int count, int y) {
  if (count < 2 || count > 18) return;
  int x = (GFX_W - count * 6) / 2;
  for (int i = 0; i < count; i++) {
    if (i == idx) {
      gfx_fill(x, y, 6, 2, true);
      x += 10;
    } else {
      gfx_fill(x, y, 2, 2, true);
      x += 6;
    }
  }
}

static void chevron(int count) {
  if (count > 1) gfx_icon(GFX_W - 5, TALL ? 22 : 9, ICON_CHEVRON);
}

void ui_carousel(const uint8_t *icon, const char *title, const char *sub, int idx, int count) {
  gfx_clear();
  gfx_icon_text_center(TALL ? 19 : 5, icon, title, GFX_BOLD);
  if (sub) gfx_text_center(TALL ? 32 : 17, sub, GFX_NORMAL);
  chevron(count);
  dots(idx, count, DOTS_Y);
}

bool ui_dashboard_at(float pos) {
  (void)pos;
  return false;
}

// Строка иконок — окно до 9 штук вокруг выбранной, если приложений много
static void dash_row(const uint8_t *const icons[], int count, int sel, const char *title,
                     const char *sub);

void ui_dashboard(const uint8_t *const all[], const char *const names[], int n, int dash_i,
                  const char *title, const char *sub) {
  (void)names;
  enum { VISIBLE = 9 };
  int first = 0, shown = n < VISIBLE ? n : VISIBLE;
  if (n > VISIBLE) {
    first = dash_i - VISIBLE / 2;
    if (first < 0) first = 0;
    if (first > n - VISIBLE) first = n - VISIBLE;
  }
  dash_row(all + first, shown, dash_i - first, title, sub);
}

static void dash_row(const uint8_t *const icons[], int count, int sel, const char *title,
                     const char *sub) {
  gfx_clear();
  // слоты одинаковой ширины по всей строке, иконки по центрам слотов
  int slot = GFX_W / (count < 5 ? 5 : count);
  int x0 = (GFX_W - slot * count) / 2;
  int y = TALL ? 17 : 0;  // верх плашки; на высоком экране сверху строка статуса
  for (int i = 0; i < count; i++) {
    int x = x0 + i * slot + (slot - 8) / 2;
    if (i == sel) {
      gfx_round_fill(x - 3, y, 14, 14);
      gfx_icon_inv(x, y + 3, icons[i]);
    } else {
      gfx_icon(x, y + 3, icons[i]);
    }
  }
  gfx_text_center(TALL ? 36 : 16, title, GFX_BOLD);
  if (sub) gfx_text_center(TALL ? 48 : 24, sub, GFX_NORMAL);
}

// Значок батареи 12x7: корпус, «носик» справа, заливка по проценту
static void battery_icon(int x, int y, int pct) {
  gfx_fill(x, y, 11, 1, true);
  gfx_fill(x, y + 6, 11, 1, true);
  gfx_fill(x, y, 1, 7, true);
  gfx_fill(x + 10, y, 1, 7, true);
  gfx_fill(x + 11, y + 2, 1, 3, true);
  gfx_fill(x + 2, y + 2, (7 * pct + 50) / 100, 3, true);
}

void ui_status(const char *left, bool ble_on, bool ble_connected, bool usb, int batt_pct) {
  if (!TALL) return;  // на 32 строках места нет
  gfx_fill(0, 0, GFX_W, 9, false);
  gfx_text(0, 0, left, GFX_NORMAL);
  int x = GFX_W - 12;
  if (batt_pct == UI_BATT_CHARGING) {  // батарейка с молнией
    battery_icon(x, 0, 0);
    gfx_fill(x + 6, 2, 2, 1, true);
    gfx_fill(x + 4, 3, 4, 1, true);
    gfx_fill(x + 4, 4, 2, 1, true);
    x -= 16;
  } else if (batt_pct >= 0) {
    char t[16];
    battery_icon(x, 0, batt_pct);
    snprintf(t, sizeof t, "%d%%", batt_pct);
    x -= gfx_text_width(t, GFX_NORMAL) + 2;
    gfx_text(x, 0, t, GFX_NORMAL);
    x -= 12;
  } else {
    x += 4;
  }
  if (usb) {
    gfx_icon(x, 0, ICON_USB);
    x -= 12;
  }
  if (ble_on) {
    if (ble_connected) {  // подключено — иконка на плашке
      gfx_round_fill(x - 2, 0, 12, 9);
      gfx_icon_inv(x, 0, ICON_BT);
    } else {
      gfx_icon(x, 0, ICON_BT);
    }
  }
  gfx_hline(0, 10, GFX_W);
}

void ui_message(const uint8_t *icon, const char *title, const char *sub) {
  gfx_clear();
  if (TALL) gfx_icon_text_center(sub ? 21 : 28, icon, title, GFX_BOLD);
  else gfx_icon_text_center(sub ? 7 : 12, icon, title, GFX_BOLD);
  if (sub) gfx_text_center(TALL ? 34 : 19, sub, GFX_NORMAL);
}

static void header(const uint8_t *icon, const char *title, int page, int pages) {
  int x = 0;
  if (icon) {
    gfx_icon(0, 0, icon);
    x = 12;
  }
  gfx_text(x, 0, title, GFX_BOLD);
  if (pages > 1) {
    char p[24];
    snprintf(p, sizeof p, "%d/%d", page + 1, pages);
    gfx_text(GFX_W - gfx_text_width(p, GFX_NORMAL), 0, p, GFX_NORMAL);
  }
}

// Разбивка значения на строки. Адреса и хеши (без пробелов) режем на равные
// куски, а число строк добиваем до целых страниц, чтобы последняя страница
// не состояла из одного символа: 64 символа на 32-строчном экране -> 4 x 16.
// Обычный текст переносим по словам.
#define MAX_LINES 64

static int split(const char *v, int start[MAX_LINES], int len[MAX_LINES]) {
  int n = (int)strlen(v), count = 0;
  if (!strchr(v, ' ')) {
    int lines = n ? (n + UI_LINE - 1) / UI_LINE : 1;
    if (lines > FIELD_LINES && lines % FIELD_LINES) lines += FIELD_LINES - lines % FIELD_LINES;
    if (lines > MAX_LINES) lines = MAX_LINES;
    int per = n ? (n + lines - 1) / lines : 1;
    for (int pos = 0; pos < n && count < MAX_LINES; pos += per, count++) {
      start[count] = pos;
      len[count] = n - pos < per ? n - pos : per;
    }
    return count ? count : 1;
  }
  for (int pos = 0; pos < n && count < MAX_LINES; count++) {
    while (v[pos] == ' ') pos++;
    int cut = pos + UI_LINE;
    if (cut >= n) {
      cut = n;
    } else {
      int c = cut;
      while (c > pos && v[c] != ' ') c--;
      if (c > pos) cut = c;  // иначе слово длиннее строки — режем как есть
    }
    start[count] = pos;
    len[count] = cut - pos;
    pos = cut;
  }
  return count ? count : 1;
}

// Адреса, хеши, xpub — группами по 4 символа, как в приложении на телефоне
// (Codec.kt groupAddr): группы считаются с начала, так что сверять можно группа
// за группой. 4 группы в строке с зазором, блок по центру экрана.
#define GROUP 4
#define GROUP_LINE 4
#define GROUP_GAP 8
#define GROUP_LINES (TALL ? 4 : 2)    // строк на страницу
#define GROUP_PITCH (TALL ? 11 : 10)  // шаг строк: хвосты букв не касаются следующей

static bool grouped(const char *v) {
  int n = 0;
  for (; v[n]; n++)
    if (!isalnum((unsigned char)v[n])) return false;
  return n >= 12;
}

static int group_lines(int n) {
  int groups = (n + GROUP - 1) / GROUP;
  return (groups + GROUP_LINE - 1) / GROUP_LINE;
}

static void draw_grouped(const char *v, int page) {
  int n = (int)strlen(v), first = page * GROUP_LINES;
  int k = group_lines(n) - first;
  if (k > GROUP_LINES) k = GROUP_LINES;
  int gw = GROUP * gfx_char_width(GFX_NORMAL) - 1;
  int x0 = (GFX_W - (GROUP_LINE * gw + (GROUP_LINE - 1) * GROUP_GAP)) / 2;
  int top = TALL ? 12 : 10, area = (TALL ? DOTS_Y - 3 : 29) - top;
  int y = top + (area - ((k - 1) * GROUP_PITCH + 9)) / 2;
  for (int l = 0; l < k; l++, y += GROUP_PITCH) {
    for (int g = 0; g < GROUP_LINE; g++) {
      int pos = ((first + l) * GROUP_LINE + g) * GROUP;
      if (pos >= n) break;
      char part[GROUP + 1];
      int len = n - pos < GROUP ? n - pos : GROUP;
      memcpy(part, v + pos, len);
      part[len] = 0;
      gfx_text(x0 + g * (gw + GROUP_GAP), y, part, GFX_NORMAL);
    }
  }
}

int ui_pages(const char *value) {
  if (grouped(value)) return (group_lines((int)strlen(value)) + GROUP_LINES - 1) / GROUP_LINES;
  int start[MAX_LINES], len[MAX_LINES];
  return (split(value, start, len) + FIELD_LINES - 1) / FIELD_LINES;
}

void ui_field(const uint8_t *icon, const char *title, const char *value,
              int page, int pages, int idx, int count) {
  gfx_clear();
  header(icon, title, page, pages);
  if (grouped(value)) {
    draw_grouped(value, page);
    dots(idx, count, TALL ? DOTS_Y : 29);
    return;
  }
  char line[UI_LINE + 1];
  int start[MAX_LINES], len[MAX_LINES];
  int lines = split(value, start, len);
  for (int l = 0; l < FIELD_LINES; l++) {
    int i = page * FIELD_LINES + l;
    if (i >= lines || !len[i]) break;
    memcpy(line, value + start[i], len[i]);
    line[len[i]] = 0;
    gfx_text(0, (TALL ? 12 : 10) + 9 * l, line, GFX_NORMAL);
  }
  dots(idx, count, TALL ? DOTS_Y : 29);
}

// OLED: суммы через запятую в поле, по страницам
static const char *balance_line(const char *text) {
  static char b[160];
  int o = 0;
  for (const char *c = text; *c && o < (int)sizeof b - 3; c++) {
    if (*c == ';') {
      b[o++] = ',';
      b[o++] = ' ';
    } else {
      b[o++] = *c;
    }
  }
  b[o] = 0;
  return b;
}

void ui_balance(const uint8_t *icon, const char *when, const char *text, int page, int pages) {
  char t[32];
  snprintf(t, sizeof t, "Balance %s", when);
  ui_field(icon, t, balance_line(text), page, pages, -1, 0);
}

int ui_balance_pages(const char *text) { return ui_pages(balance_line(text)); }

void ui_anim(int kind) { (void)kind; }  // на OLED кадр меняется сразу
void ui_anim_btn(int btn) { (void)btn; }

void ui_receive(const uint8_t *icon, const char *title, const char *addr, int page, int pages) {
  ui_field(icon, title, addr, page, pages, -1, 0);
}

int ui_receive_pages(const char *addr) { return ui_pages(addr); }

void ui_amount(const uint8_t *icon, const char *title, const char *value, int idx, int count) {
  if (strlen(value) > 10) {
    ui_field(icon, title, value, 0, 1, idx, count);
    return;
  }
  gfx_clear();
  header(icon, title, 0, 1);
  gfx_text_center(TALL ? 23 : 11, value, GFX_BIG);
  dots(idx, count, TALL ? DOTS_Y : 29);
}

void ui_progress(const uint8_t *icon, const char *title, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  gfx_clear();
  gfx_icon_text_center(TALL ? 17 : 5, icon, title, GFX_BOLD);
  // рамка со скруглёнными углами и заливка
  const int x = 14, y = TALL ? 33 : 19, w = GFX_W - 28, h = TALL ? 9 : 7;
  gfx_fill(x + 1, y, w - 2, 1, true);
  gfx_fill(x + 1, y + h - 1, w - 2, 1, true);
  gfx_fill(x, y + 1, 1, h - 2, true);
  gfx_fill(x + w - 1, y + 1, 1, h - 2, true);
  gfx_fill(x + 2, y + 2, (w - 4) * percent / 100, h - 4, true);
}

// Ввод PIN: заголовок, 4 клетки (набранные — залиты), под ними — шкала из 9 делений по 0.5 с
// (пока кнопку держат) или подсказка «клик — ввести, удержание — заново»
void ui_pin(const char *title, int entered, int level, bool confirm) {
  gfx_clear();
  gfx_icon_text_center(TALL ? 4 : 0, ICON_LOCK, title, GFX_BOLD);
  const int cw = 14, gap = 8, x0 = (GFX_W - (4 * cw + 3 * gap)) / 2, y = TALL ? 18 : 10;
  for (int i = 0; i < 4; i++) {
    int x = x0 + i * (cw + gap);
    gfx_fill(x, y + cw - 1, cw, 1, true);  // черта под клеткой
    if (i < entered) gfx_round_fill(x + 3, y + 3, cw - 6, cw - 6);
  }
  if (confirm) {
    gfx_text_center(TALL ? 38 : 26, "click - enter", GFX_NORMAL);
    if (TALL) gfx_text_center(50, "hold - again", GFX_NORMAL);
    return;
  }
  const int sw = 10, sg = 3, sx = (GFX_W - (9 * sw + 8 * sg)) / 2, sy = TALL ? 42 : 27;
  for (int k = 0; k < 9; k++) {
    int x = sx + k * (sw + sg);
    if (k < level) gfx_fill(x, sy, sw, 7, true);
    else gfx_fill(x, sy + 6, sw, 1, true);
  }
  if (TALL && level < 0) gfx_text_center(54, "longer = bigger", GFX_NORMAL);
}

void ui_word(const char *title, const char *word, int idx, int count) {
  gfx_clear();
  gfx_text_center(TALL ? 8 : 0, title, GFX_NORMAL);
  if (gfx_text_width(word, GFX_BIG) <= GFX_W) gfx_text_center(TALL ? 24 : 10, word, GFX_BIG);
  else gfx_text_center(TALL ? 28 : 14, word, GFX_BOLD);
  dots(idx, count, TALL ? DOTS_Y : 29);
}

void ui_splash(const char *version, const char *net) {
  char sub[32];
  gfx_clear();
  // «MicroWallet» крупно не влезает в 128 точек (130) — сжимаем интервал на точку
  static const char NAME[] = "MicroWallet";
  int n = (int)sizeof NAME - 1, x = (GFX_W - (n * 11 - 1)) / 2;
  for (int i = 0; i < n; i++, x += 11) {
    char ch[2] = {NAME[i], 0};
    gfx_text(x, TALL ? 14 : 3, ch, GFX_BIG);
  }
  snprintf(sub, sizeof sub, "v%s  %s", version, net);
  gfx_text_center(TALL ? 40 : 23, sub, GFX_NORMAL);
}
