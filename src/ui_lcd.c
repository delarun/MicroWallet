// ui_lcd.c — цветной интерфейс для экрана 320x172 (Waveshare 1.47): те же экраны ui.h, что и
// ui.c для OLED 128x64, но в цвете, сглаженными шрифтами и с крупными элементами под палец.
// Навигация — как в button.h: края экрана и свайпы листают, середина выбирает.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "button.h"
#include "draw.h"
#include "icons.h"
#include "qrcodegen.h"
#include "ui.h"

#define BG RGB(10, 13, 20)
#define CARD RGB(28, 34, 46)
#define TEXT RGB(240, 243, 248)
#define MUTED RGB(140, 150, 168)
#define DIM RGB(70, 79, 96)
#define ACCENT RGB(64, 140, 255)
#define GREEN RGB(46, 190, 113)
#define RED RGB(240, 82, 74)
#define AMBER RGB(245, 166, 36)
#define ALT RGB(150, 196, 255)  // каждая вторая группа символов адреса

#define CX (LCD_W / 2)
#define QR_MAX_VERSION 10  // до 271 символа при ECC L — с запасом для любого адреса
#define DOTS_Y 160
#define MARGIN 12

// ---------- общие детали ----------

// Тёмный текст на светлом круге, светлый — на тёмном
static uint16_t on_color(uint16_t c) {
  int lum = (c >> 11) * 2 * 299 + (c >> 5 & 63) * 587 + (c & 31) * 2 * 114;  // ~0..63000
  return lum > 38000 ? RGB(16, 18, 24) : TEXT;
}

static uint16_t icon_accent(const uint8_t *icon) {
  if (icon == ICON_CHECK) return GREEN;
  if (icon == ICON_CROSS || icon == ICON_TRASH) return RED;
  if (icon == ICON_WARN) return AMBER;
  if (icon == ICON_BACK || icon == ICON_GEAR || icon == ICON_INFO) return DIM;
  return ACCENT;
}

// Цвета монет — по названию приложения
static uint16_t coin_color(const char *name) {
  static const struct {
    const char *name;
    uint16_t c;
  } k[] = {
      {"Bitcoin", RGB(247, 147, 26)}, {"Litecoin", RGB(52, 93, 157)}, {"Ethereum", RGB(98, 126, 234)},
      {"BNB", RGB(240, 185, 11)},     {"TRON", RGB(235, 0, 41)},      {"TON", RGB(0, 152, 234)},
      {"Solana", RGB(153, 69, 255)},  {"Pearl", RGB(20, 184, 166)},   {"Settings", DIM},
  };
  for (size_t i = 0; i < sizeof k / sizeof *k; i++)
    if (!strncmp(name, k[i].name, strlen(k[i].name))) return k[i].c;
  return ACCENT;
}

// Значок в круге: иконка 8x8, увеличенная до scale, рисунком по центру круга
static void badge(int cx, int cy, int r, const uint8_t *icon, int scale, uint16_t c) {
  d_circle(cx, cy, r, c);
  if (icon) d_icon_c(cx, cy, icon, scale, on_color(c));
}

// Стрелка-подсказка у края (10x17): туда листает касание. Всегда посередине высоты экрана
// и с отступом от края — на одном месте на всех экранах
#define CHEV_PAD 16
#define CHEV_Y (LCD_H / 2 - 8)

static void chevron(int x, int y, bool right, uint16_t c) {
  for (int i = 0; i < 8; i++) {
    int dx = right ? i : 7 - i;
    d_fill(x + dx, y + i, 3, 2, c);
    d_fill(x + dx, y + 15 - i, 3, 2, c);
  }
}

static void chevrons(int count) {
  if (count < 2) return;
  lcd_anim_area.slide_x0 = CHEV_PAD + 10 + 6;  // листается только середина, стрелки стоят
  lcd_anim_area.slide_x1 = LCD_W - CHEV_PAD - 10 - 6;
  chevron(CHEV_PAD, CHEV_Y, false, DIM);
  chevron(LCD_W - CHEV_PAD - 10, CHEV_Y, true, DIM);
}

// Точки внизу: текущая — вытянутая и яркая
static void dots(int idx, int count) {
  if (count < 2 || count > 24 || idx < 0) return;
  lcd_anim_area.fixed_y0 = DOTS_Y;  // точки не едут вместе с экраном
  lcd_anim_area.fixed_y1 = DOTS_Y + 6;
  const int d = 6, gap = 6, cur = 16;
  int w = (count - 1) * (d + gap) + cur, x = CX - w / 2;
  for (int i = 0; i < count; i++) {
    if (i == idx) {
      d_rrect(x, DOTS_Y, cur, d, 3, TEXT);
      x += cur + gap;
    } else {
      d_rrect(x, DOTS_Y, d, d, 3, DIM);
      x += d + gap;
    }
  }
}

// Текст, который не влезает в ширину, — мельче
static const font_t *fit(const char *s, int w, const font_t *a, const font_t *b) {
  return d_text_w(s, a) <= w ? a : b;
}

void ui_anim(int kind) { lcd_anim = kind; }  // UI_ANIM_* == ANIM_* (draw.h)

void ui_anim_btn(int btn) {
  ui_anim(btn == BTN_TAP ? UI_ANIM_NEXT : btn == BTN_PREV ? UI_ANIM_PREV : UI_ANIM_FADE);
}

// ---------- экраны ----------

// ---------- главный экран: карусель ----------
// Кружки монет на одной линии через DASH_STEP точек; выбранный — крупный и в цвете монеты,
// соседи меньше и приглушены, дальше двух позиций — не видно. Положение дробное: палец тянет
// ленту (ui_dashboard_at), отпустил — доезжает до ближайшей. Лента бесконечная: за последним
// пунктом снова первый, положение берётся по модулю числа пунктов.
#define DASH_Y 74
#define DASH_STEP 82
#define DASH_R0 34  // выбранный
#define DASH_R1 22  // соседи
#define STATUS_H 24

static struct {
  const uint8_t *icons[UI_DASH_MAX];
  const char *names[UI_DASH_MAX];
  int count, sel;
  char title[32], sub[48];
  bool has_sub;
} dash;

static int wrap(int i, int n) { return (i % n + n) % n; }

static void carousel(float pos) {
  d_fill(0, STATUS_H, LCD_W, LCD_H - STATUS_H, BG);
  lcd_anim_area = (anim_area_t){0, LCD_W, 0, 0};
  // дальние — сначала, ближний к центру — поверх
  int c = (int)floorf(pos + 0.5f);
  const int order[5] = {c - 2, c + 2, c - 1, c + 1, c};
  for (int k = 0; k < 5; k++) {
    int i = wrap(order[k], dash.count);
    float p = (float)order[k] - pos, ap = fabsf(p);
    if (ap >= 2) continue;
    float r = ap <= 1 ? DASH_R0 - (DASH_R0 - DASH_R1) * ap : DASH_R1 * (2 - ap);
    if (r < 3) continue;
    uint16_t full = coin_color(dash.names[i]), dim = d_mix(full, BG, 70);
    float t = ap < 1 ? ap : 1;
    uint16_t col = d_mix(dim, full, (int)(255 * t));
    float size = ap <= 1 ? 32 - 8 * ap : 24 * (2 - ap);  // иконка 32 -> 24 -> 0 точек
    int x = CX + (int)lroundf(p * DASH_STEP);
    d_circle(x, DASH_Y, (int)lroundf(r), col);
    d_icon_sz(x, DASH_Y, dash.icons[i], size, d_mix(on_color(col), col, ap < 1 ? 255 - (int)(55 * ap) : 200));
  }
  // всё, что уехало за стрелки, — срезать, стрелки поверх
  const int wx0 = CHEV_PAD + 10 + 4, wx1 = LCD_W - CHEV_PAD - 10 - 4;
  d_fill(0, DASH_Y - DASH_R0 - 1, wx0, 2 * DASH_R0 + 3, BG);
  d_fill(wx1, DASH_Y - DASH_R0 - 1, LCD_W - wx1, 2 * DASH_R0 + 3, BG);
  chevrons(dash.count);
  // название ближайшего: тает к середине пути и проявляется у следующего
  int near = wrap(c, dash.count);
  float f = fabsf(pos - (float)c);
  int a = f >= 0.5f ? 0 : (int)(255 * (1 - 2 * f));
  const char *name = near == dash.sel ? dash.title : dash.names[near];
  d_text_c(CX, 106, name, fit(name, LCD_W - 2 * MARGIN, &font_title, &font_body), d_mix(TEXT, BG, a));
  if (near == dash.sel && dash.has_sub) d_text_c(CX, 134, dash.sub, &font_small, d_mix(MUTED, BG, a));
  dots(near, dash.count);
}

void ui_dashboard(const uint8_t *const icons[], const char *const names[], int count, int sel,
                  const char *title, const char *sub) {
  if (count > UI_DASH_MAX) count = UI_DASH_MAX;
  memcpy(dash.icons, icons, count * sizeof *icons);
  memcpy(dash.names, names, count * sizeof *names);
  dash.count = count;
  dash.sel = sel;
  snprintf(dash.title, sizeof dash.title, "%s", title);
  dash.has_sub = sub != NULL;
  snprintf(dash.sub, sizeof dash.sub, "%s", sub ? sub : "");
  d_clear(BG);
  carousel((float)sel);
}

bool ui_dashboard_at(float pos) {
  if (!dash.count) return false;
  carousel(pos);  // строка статуса в кадре осталась от последнего ui_status
  return true;
}

// Батарейка 24x12 с «носиком»; заливка по проценту, красная ниже 20 %
static int battery(int x, int y, int pct, bool charging) {
  d_rrect(x, y, 22, 12, 3, MUTED);
  d_rrect(x + 1, y + 1, 20, 10, 2, BG);
  d_fill(x + 22, y + 4, 2, 4, MUTED);
  if (charging) {
    d_fill(x + 3, y + 3, 16, 6, GREEN);
  } else if (pct >= 0) {
    int w = (16 * pct + 50) / 100;
    d_fill(x + 3, y + 3, w ? w : 1, 6, pct < 20 ? RED : TEXT);
  }
  return x;
}

void ui_status(const char *left, bool ble_on, bool ble_connected, bool usb, int batt_pct) {
  d_fill(0, 0, LCD_W, 24, BG);
  uint16_t nc = strcmp(left, "mainnet") ? AMBER : GREEN;
  int w = d_text_w(left, &font_small) + 16;
  d_rrect(MARGIN, 3, w, 18, 9, d_mix(nc, BG, 60));
  d_text(MARGIN + 8, 1, left, &font_small, nc);
  int x = LCD_W - MARGIN - 24;
  if (batt_pct == UI_BATT_CHARGING) {
    battery(x, 6, 0, true);
    x -= 8;
  } else if (batt_pct >= 0) {
    char t[16];
    battery(x, 6, batt_pct, false);
    snprintf(t, sizeof t, "%d%%", batt_pct);
    x -= d_text_w(t, &font_small) + 6;
    d_text(x, 1, t, &font_small, MUTED);
    x -= 6;
  } else {
    x += 16;
  }
  if (usb) {
    x -= 18;
    d_icon(x, 4, ICON_USB, 2, MUTED);
  }
  if (ble_on) {
    x -= 20;
    d_icon(x, 4, ICON_BT, 2, ble_connected ? ACCENT : DIM);
  }
}

void ui_carousel(const uint8_t *icon, const char *title, const char *sub, int idx, int count) {
  d_clear(BG);
  badge(CX, 54, 30, icon, 4, icon_accent(icon));
  chevrons(count);
  d_text_c(CX, 92, title, fit(title, LCD_W - 2 * MARGIN, &font_title, &font_body), TEXT);
  if (sub) d_text_c(CX, 124, sub, fit(sub, LCD_W - 2 * MARGIN, &font_body, &font_small), MUTED);
  dots(idx, count);
}

void ui_message(const uint8_t *icon, const char *title, const char *sub) {
  d_clear(BG);
  int y = sub ? 0 : 10;
  badge(CX, 56 + y, 30, icon, 4, icon_accent(icon));
  d_text_c(CX, 94 + y, title, fit(title, LCD_W - 2 * MARGIN, &font_title, &font_body), TEXT);
  if (sub) d_text_c(CX, 126, sub, fit(sub, LCD_W - 2 * MARGIN, &font_body, &font_small), MUTED);
}

static void header(const uint8_t *icon, const char *title, int page, int pages) {
  int x = MARGIN;
  if (icon) {
    d_rrect(x, 6, 24, 24, 6, icon_accent(icon) == DIM ? CARD : d_mix(icon_accent(icon), BG, 70));
    d_icon_c(x + 12, 18, icon, 2, TEXT);
    x += 32;
  }
  d_text(x, 3, title, &font_body, TEXT);
  if (pages > 1) {
    char p[24];
    snprintf(p, sizeof p, "%d/%d", page + 1, pages);
    d_text(LCD_W - MARGIN - d_text_w(p, &font_small), 6, p, &font_small, MUTED);
  }
  d_fill(MARGIN, 36, LCD_W - 2 * MARGIN, 1, CARD);
}

// Адреса, хеши, xpub — моноширинно, группами по 4 символа (как в приложении на телефоне),
// группы через одну другим цветом: удобно сверять группа за группой
#define GROUP 4
#define GROUPS_PER_LINE 5
#define GROUP_LINES 4
#define GROUP_PITCH 28

static bool grouped(const char *v) {
  int n = 0;
  for (; v[n]; n++)
    if (!isalnum((unsigned char)v[n])) return false;
  return n >= 12;
}

static int group_lines(int n) {
  int groups = (n + GROUP - 1) / GROUP;
  return (groups + GROUPS_PER_LINE - 1) / GROUPS_PER_LINE;
}

static void draw_grouped(const char *v, int page) {
  int n = (int)strlen(v), first = page * GROUP_LINES;
  int k = group_lines(n) - first;
  if (k > GROUP_LINES) k = GROUP_LINES;
  int gw = d_text_wn("0000", GROUP, &font_mono), gap = 12;
  int x0 = CX - (GROUPS_PER_LINE * gw + (GROUPS_PER_LINE - 1) * gap) / 2;
  int y = 40 + (GROUP_LINES - k) * GROUP_PITCH / 2;
  for (int l = 0; l < k; l++, y += GROUP_PITCH)
    for (int g = 0; g < GROUPS_PER_LINE; g++) {
      int gi = (first + l) * GROUPS_PER_LINE + g, pos = gi * GROUP;
      if (pos >= n) break;
      d_text_n(x0 + g * (gw + gap), y, v + pos, n - pos < GROUP ? n - pos : GROUP, &font_mono,
               gi & 1 ? ALT : TEXT);
    }
}

// Обычный текст — по словам в ширину экрана; слово длиннее строки режется
#define TEXT_LINES 4
#define TEXT_PITCH 26
#define MAX_LINES 64

static int split(const char *v, int start[MAX_LINES], int len[MAX_LINES]) {
  const int width = LCD_W - 2 * MARGIN;
  int n = (int)strlen(v), count = 0;
  for (int pos = 0; pos < n && count < MAX_LINES; count++) {
    while (v[pos] == ' ') pos++;
    int end = pos, last_space = -1;
    while (end < n && d_text_wn(v + pos, end - pos + 1, &font_body) <= width) {
      if (v[end] == ' ') last_space = end;
      end++;
    }
    if (end < n && last_space > pos) end = last_space;
    if (end == pos) end = pos + 1;
    start[count] = pos;
    len[count] = end - pos;
    pos = end;
  }
  return count ? count : 1;
}

int ui_pages(const char *value) {
  if (grouped(value)) return (group_lines((int)strlen(value)) + GROUP_LINES - 1) / GROUP_LINES;
  int start[MAX_LINES], len[MAX_LINES];
  return (split(value, start, len) + TEXT_LINES - 1) / TEXT_LINES;
}

void ui_field(const uint8_t *icon, const char *title, const char *value, int page, int pages, int idx,
              int count) {
  d_clear(BG);
  header(icon, title, page, pages);
  if (grouped(value)) {
    draw_grouped(value, page);
  } else {
    int start[MAX_LINES] = {0}, len[MAX_LINES] = {0};
    int lines = split(value, start, len), first = page * TEXT_LINES;
    int k = lines - first < TEXT_LINES ? lines - first : TEXT_LINES;
    int y = 42 + (TEXT_LINES - k) * TEXT_PITCH / 2;
    for (int l = 0; l < k; l++, y += TEXT_PITCH) {
      const char *t = value + start[first + l];
      int x = lines == 1 ? CX - d_text_wn(t, len[first + l], &font_body) / 2 : MARGIN;  // одна строка — по центру
      d_text_n(x, y, t, len[first + l], &font_body, TEXT);
    }
  }
  dots(idx, count);
}

// Получение: слева QR-код адреса (белое поле с тихой зоной), справа название и адрес
// моноширинно группами по 4. Всё на одной странице.
#define QR_BOX 160  // наибольшая сторона белого квадрата
#define QR_QUIET 2  // тихая зона, модулей

int ui_receive_pages(const char *addr) {
  (void)addr;
  return 1;
}

// ---------- баланс: список сумм ----------
// Строка на сумму: точка (монета сети — цветная), символ слева, сумма крупно справа;
// по BAL_ROWS строк на странице, страницы — точками внизу
#define BAL_ROWS 4
#define BAL_TOP 42
#define BAL_ROW_H 28

static int balance_entries(const char *text, const char *start[16], int len[16]) {
  int n = 0;
  for (const char *p = text; *p && n < 16;) {
    int l = (int)strcspn(p, ";");
    if (l) {
      start[n] = p;
      len[n++] = l;
    }
    p += l + (p[l] == ';');
  }
  return n;
}

int ui_balance_pages(const char *text) {
  const char *st[16];
  int ln[16], n = balance_entries(text, st, ln);
  return n ? (n + BAL_ROWS - 1) / BAL_ROWS : 1;
}

void ui_balance(const uint8_t *icon, const char *when, const char *text, int page, int pages) {
  d_clear(BG);
  int x = MARGIN;
  if (icon) {
    d_rrect(x, 6, 24, 24, 6, d_mix(ACCENT, BG, 70));
    d_icon_c(x + 12, 18, icon, 2, TEXT);
    x += 32;
  }
  d_text(x, 3, "Balance", &font_body, TEXT);
  d_text(LCD_W - MARGIN - d_text_w(when, &font_small), 6, when, &font_small, MUTED);
  d_fill(MARGIN, 36, LCD_W - 2 * MARGIN, 1, CARD);
  const char *st[16];
  int ln[16], n = balance_entries(text, st, ln);
  for (int r = 0; r < BAL_ROWS; r++) {
    int i = page * BAL_ROWS + r, ry = BAL_TOP + r * BAL_ROW_H;
    if (i >= n) break;
    // «12.5 USDT»: сумма — до последнего пробела, символ — после
    int sp = ln[i];
    while (sp > 0 && st[i][sp - 1] != ' ') sp--;
    char amt[48], sym[24];
    snprintf(amt, sizeof amt, "%.*s", sp ? sp - 1 : ln[i], st[i]);
    snprintf(sym, sizeof sym, "%.*s", sp ? ln[i] - sp : 0, st[i] + sp);
    if (r) d_fill(MARGIN, ry, LCD_W - 2 * MARGIN, 1, CARD);
    d_circle(MARGIN + 6, ry + BAL_ROW_H / 2, 5, i == 0 ? ACCENT : DIM);
    d_text(MARGIN + 18, ry + 1, sym, &font_body, i == 0 ? TEXT : MUTED);
    int room = LCD_W - MARGIN - (MARGIN + 18 + d_text_w(sym, &font_body) + 12);
    const font_t *f = d_text_w(amt, &font_title) <= room ? &font_title : &font_body;
    d_text(LCD_W - MARGIN - d_text_w(amt, f), f == &font_title ? ry - 3 : ry + 1, amt, f, TEXT);
  }
  dots(page, pages);
}

void ui_receive(const uint8_t *icon, const char *title, const char *addr, int page, int pages) {
  (void)page;
  (void)pages;
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
  if (!qrcodegen_encodeText(addr, tmp, qr, qrcodegen_Ecc_LOW, 1, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
    ui_field(icon, title, addr, 0, ui_pages(addr), -1, 0);  // не влез в QR — просто текстом
    return;
  }
  d_clear(BG);
  int n = qrcodegen_getSize(qr), scale = QR_BOX / (n + 2 * QR_QUIET), box = (n + 2 * QR_QUIET) * scale;
  int bx = (LCD_H - box) / 2, by = (LCD_H - box) / 2;
  d_rrect(bx, by, box, box, 6, RGB(255, 255, 255));
  int qx = bx + QR_QUIET * scale, qy = by + QR_QUIET * scale;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (qrcodegen_getModule(qr, x, y)) d_fill(qx + x * scale, qy + y * scale, scale, scale, RGB(0, 0, 0));

  // справа: значок, название, адрес
  int rx = bx + box + 14, rw = LCD_W - MARGIN - rx;
  if (icon) {
    d_rrect(rx, by, 24, 24, 6, d_mix(ACCENT, BG, 70));
    d_icon_c(rx + 12, by + 12, icon, 2, TEXT);
  }
  d_text(rx + (icon ? 32 : 0), by - 2, title, fit(title, rw - 32, &font_body, &font_small), TEXT);
  int len = (int)strlen(addr), gw = d_text_wn("0000", 4, &font_mono_s), gap = 8;
  int per = (rw + gap) / (gw + gap);  // групп в строке
  if (per < 1) per = 1;
  int lines = ((len + 3) / 4 + per - 1) / per, pitch = 20;
  int y = by + 32 + (box - 32 - lines * pitch) / 2;
  if (y < by + 30) y = by + 30;
  for (int l = 0; l < lines; l++, y += pitch)
    for (int g = 0; g < per; g++) {
      int gi = l * per + g, pos = gi * 4;
      if (pos >= len) break;
      d_text_n(rx + g * (gw + gap), y, addr + pos, len - pos < 4 ? len - pos : 4, &font_mono_s, gi & 1 ? ALT : TEXT);
    }
}

void ui_amount(const uint8_t *icon, const char *title, const char *value, int idx, int count) {
  const int width = LCD_W - 2 * MARGIN;
  if (d_text_w(value, &font_title) > width) {
    ui_field(icon, title, value, 0, 1, idx, count);
    return;
  }
  d_clear(BG);
  header(icon, title, 0, 1);
  if (d_text_w(value, &font_big) <= width) d_text_c(CX, 62, value, &font_big, TEXT);
  else d_text_c(CX, 74, value, &font_title, TEXT);
  dots(idx, count);
}

void ui_progress(const uint8_t *icon, const char *title, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  d_clear(BG);
  badge(CX, 50, 28, icon, 4, icon_accent(icon));
  d_text_c(CX, 84, title, &font_title, TEXT);
  const int x = 40, y = 128, w = LCD_W - 80, h = 12;
  d_rrect(x, y, w, h, h / 2, CARD);
  int f = w * percent / 100;
  if (f >= h) d_rrect(x, y, f, h, h / 2, ACCENT);
}

// Ввод PIN кнопкой: 4 клетки, под ними шкала из 9 делений по 0.5 с (пока кнопку держат)
void ui_pin(const char *title, int entered, int level, bool confirm) {
  d_clear(BG);
  int tw = d_text_w(title, &font_title), x = CX - (tw + 26) / 2;
  d_icon(x, 12, ICON_LOCK, 2, ACCENT);
  d_text(x + 26, 0, title, &font_title, TEXT);
  const int cw = 42, ch = 46, gap = 14, x0 = CX - (4 * cw + 3 * gap) / 2, y = 42;
  for (int i = 0; i < 4; i++) {
    int bx = x0 + i * (cw + gap);
    d_rrect(bx, y, cw, ch, 8, i == entered && !confirm ? d_mix(ACCENT, CARD, 90) : CARD);
    if (i < entered) d_circle(bx + cw / 2, y + ch / 2, 8, TEXT);
  }
  if (confirm) {
    d_text_c(CX, 106, "click - enter", &font_body, TEXT);
    d_text_c(CX, 134, "hold - start again", &font_small, MUTED);
    return;
  }
  const int sw = 22, sg = 6, sx = CX - (9 * sw + 8 * sg) / 2, sy = 110;
  for (int k = 0; k < 9; k++) d_rrect(sx + k * (sw + sg), sy, sw, 12, 4, k < level ? ACCENT : CARD);
  if (level < 0) d_text_c(CX, 134, "press BOOT: longer = bigger", &font_small, MUTED);
}

void ui_word(const char *title, const char *word, int idx, int count) {
  d_clear(BG);
  chevrons(count);
  d_text_c(CX, 10, title, &font_body, MUTED);
  d_text_c(CX, 56, word, fit(word, LCD_W - 60, &font_big, &font_title), TEXT);
  dots(idx, count);
}

void ui_splash(const char *version, const char *net) {
  char sub[40];
  d_clear(BG);
  int w = d_text_w("MicroWallet", &font_big), x = CX - w / 2;
  x = d_text(x, 40, "Micro", &font_big, ACCENT);
  d_text(x, 40, "Wallet", &font_big, TEXT);
  snprintf(sub, sizeof sub, "v%s  %s", version, net);
  d_text_c(CX, 104, sub, &font_body, MUTED);
}
