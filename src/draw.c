#include "draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint16_t *lcd_fb;
int lcd_anim;
anim_area_t lcd_anim_area = {0, LCD_W, 0, 0};

bool draw_init(void) {
  if (!lcd_fb) lcd_fb = calloc(LCD_W * LCD_H, sizeof *lcd_fb);  // больше 16 КБ — в PSRAM
  return lcd_fb != NULL;
}

void d_clear(uint16_t c) {
  lcd_anim_area = (anim_area_t){0, LCD_W, 0, 0};
  for (int i = 0; i < LCD_W * LCD_H; i++) lcd_fb[i] = c;
}

void d_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) w += x, x = 0;
  if (y < 0) h += y, y = 0;
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  for (int j = 0; j < h; j++) {
    uint16_t *p = lcd_fb + (y + j) * LCD_W + x;
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

uint16_t d_mix(uint16_t fg, uint16_t bg, int a) {
  if (a >= 255) return fg;
  if (a <= 0) return bg;
  int r = ((fg >> 11) * a + (bg >> 11) * (255 - a)) / 255;
  int g = ((fg >> 5 & 63) * a + (bg >> 5 & 63) * (255 - a)) / 255;
  int b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
  return (uint16_t)(r << 11 | g << 5 | b);
}

static void blend(int x, int y, uint16_t c, int a) {
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H || a <= 0) return;
  uint16_t *p = lcd_fb + y * LCD_W + x;
  *p = d_mix(c, *p, a);
}

// Покрытие точки кругом радиуса r (с центром в cx, cy): 4x4 подвыборки
static int coverage(float px, float py, float cx, float cy, float r) {
  int n = 0;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float dx = px + (i + 0.5f) / 4 - cx, dy = py + (j + 0.5f) / 4 - cy;
      n += dx * dx + dy * dy <= r * r;
    }
  return n * 255 / 16;
}

void d_circle(int cx, int cy, int r, uint16_t c) {
  for (int y = cy - r - 1; y <= cy + r; y++)
    for (int x = cx - r - 1; x <= cx + r; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
      if (d < r - 1) blend(x, y, c, 255);
      else if (d < r + 1) blend(x, y, c, coverage((float)x, (float)y, (float)cx, (float)cy, (float)r));
    }
}

void d_rrect(int x, int y, int w, int h, int r, uint16_t c) {
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  d_fill(x + r, y, w - 2 * r, h, c);
  d_fill(x, y + r, r, h - 2 * r, c);
  d_fill(x + w - r, y + r, r, h - 2 * r, c);
  // углы — четверти круга
  const int cxs[2] = {x + r, x + w - r}, cys[2] = {y + r, y + h - r};
  for (int k = 0; k < 4; k++) {
    int ccx = cxs[k & 1], ccy = cys[k >> 1];
    int x0 = k & 1 ? ccx : x, y0 = k >> 1 ? ccy : y;
    for (int j = 0; j < r; j++)
      for (int i = 0; i < r; i++)
        blend(x0 + i, y0 + j, c, coverage((float)(x0 + i), (float)(y0 + j), (float)ccx, (float)ccy, (float)r));
  }
}

static const glyph_t *glyph(const font_t *f, char ch) {
  unsigned char u = (unsigned char)ch;
  return &f->g[(u < 32 || u > 126 ? '?' : u) - 32];
}

int d_text_n(int x, int y, const char *s, int n, const font_t *f, uint16_t c) {
  for (int k = 0; k < n && s[k]; k++) {
    const glyph_t *g = glyph(f, s[k]);
    const uint8_t *bits = f->bits + g->off;
    for (int j = 0; j < g->h; j++)
      for (int i = 0; i < g->w; i++) {
        int idx = j * g->w + i, a = bits[idx >> 1] >> (idx & 1 ? 0 : 4) & 15;
        if (a) blend(x + g->x + i, y + g->y + j, c, a * 17);
      }
    x += g->adv;
  }
  return x;
}

int d_text(int x, int y, const char *s, const font_t *f, uint16_t c) {
  return d_text_n(x, y, s, (int)strlen(s), f, c);
}

int d_text_wn(const char *s, int n, const font_t *f) {
  int w = 0;
  for (int k = 0; k < n && s[k]; k++) w += glyph(f, s[k])->adv;
  return w;
}

int d_text_w(const char *s, const font_t *f) { return d_text_wn(s, (int)strlen(s), f); }

void d_text_c(int cx, int y, const char *s, const font_t *f, uint16_t c) {
  d_text(cx - d_text_w(s, f) / 2, y, s, f, c);
}

// Рисунок в клетке 8x8 часто смещён (7 точек шириной, пустая строка снизу): центрируем
// по занятым точкам, а не по клетке
void d_icon_c(int cx, int cy, const uint8_t icon[8], int scale, uint16_t c) {
  int x0 = 8, x1 = -1, y0 = 8, y1 = -1;
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 8; b++)
      if (icon[r] >> (7 - b) & 1) {
        if (b < x0) x0 = b;
        if (b > x1) x1 = b;
        if (r < y0) y0 = r;
        if (r > y1) y1 = r;
      }
  if (x1 < 0) return;
  d_icon(cx - (x0 + x1 + 1) * scale / 2, cy - (y0 + y1 + 1) * scale / 2, icon, scale, c);
}

// То же с дробным масштабом (карусель: иконка плавно растёт и уменьшается)
void d_icon_sz(int cx, int cy, const uint8_t icon[8], float size, uint16_t c) {
  float f = size / 8;
  int x0 = 8, x1 = -1, y0 = 8, y1 = -1;
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 8; b++)
      if (icon[r] >> (7 - b) & 1) {
        if (b < x0) x0 = b;
        if (b > x1) x1 = b;
        if (r < y0) y0 = r;
        if (r > y1) y1 = r;
      }
  if (x1 < 0 || f <= 0) return;
  float ox = cx - (x0 + x1 + 1) * f / 2, oy = cy - (y0 + y1 + 1) * f / 2;
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 8; b++)
      if (icon[r] >> (7 - b) & 1) {
        int xa = (int)lroundf(ox + b * f), xb = (int)lroundf(ox + (b + 1) * f);
        int ya = (int)lroundf(oy + r * f), yb = (int)lroundf(oy + (r + 1) * f);
        d_fill(xa, ya, xb - xa, yb - ya, c);
      }
}

void d_icon(int x, int y, const uint8_t icon[8], int scale, uint16_t c) {
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 8; b++)
      if (icon[r] >> (7 - b) & 1) d_fill(x + b * scale, y + r * scale, scale, scale, c);
}

bool d_diff_rows(const uint16_t *a, const uint16_t *b, int *y0, int *y1) {
  const size_t row = LCD_W * sizeof *a;
  int top = 0, bot = LCD_H;
  while (top < LCD_H && !memcmp(a + top * LCD_W, b + top * LCD_W, row)) top++;
  if (top == LCD_H) return false;
  while (bot > top && !memcmp(a + (bot - 1) * LCD_W, b + (bot - 1) * LCD_W, row)) bot--;
  *y0 = top;
  *y1 = bot;
  return true;
}

void d_anim_frame(const uint16_t *from, const uint16_t *to, int kind, int t, int y0, int y1, uint16_t *out) {
  int u = 256 - t, e = 256 - u * u / 256 * u / 256;  // замедление к концу (кубическое)
  const anim_area_t *a = &lcd_anim_area;
  int x0 = a->slide_x0, w = a->slide_x1 - a->slide_x0, off = w * e / 256;
  for (int y = y0; y < y1; y++) {
    const uint16_t *f = from + y * LCD_W + x0, *n = to + y * LCD_W + x0;
    uint16_t *o = out + y * LCD_W;
    if (kind != ANIM_FADE) {  // вне окна и в неподвижных строках — сразу новый кадр
      memcpy(o, to + y * LCD_W, LCD_W * sizeof *o);
      if (y >= a->fixed_y0 && y < a->fixed_y1) continue;
      o += x0;
    }
    if (kind == ANIM_NEXT) {
      memcpy(o, f + off, (w - off) * sizeof *o);
      memcpy(o + w - off, n, off * sizeof *o);
    } else if (kind == ANIM_PREV) {
      memcpy(o, n + w - off, off * sizeof *o);
      memcpy(o + off, f, (w - off) * sizeof *o);
    } else {
      o = out + y * LCD_W;
      f = from + y * LCD_W;
      n = to + y * LCD_W;
      for (int x = 0; x < LCD_W; x++) o[x] = d_mix(n[x], f[x], e);
    }
  }
}
