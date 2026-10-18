#include "gfx.h"

#include <string.h>

#include "font6x10.h"

uint8_t gfx_fb[GFX_W * GFX_H / 8];

void gfx_clear(void) { memset(gfx_fb, 0, sizeof gfx_fb); }

void gfx_pixel(int x, int y, bool on) {
  if (x < 0 || x >= GFX_W || y < 0 || y >= GFX_H) return;
  uint8_t *b = &gfx_fb[(y >> 3) * GFX_W + x];
  uint8_t m = (uint8_t)(1u << (y & 7));
  *b = on ? (*b | m) : (*b & ~m);
}

void gfx_fill(int x, int y, int w, int h, bool on) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) gfx_pixel(i, j, on);
}

void gfx_hline(int x, int y, int w) { gfx_fill(x, y, w, 1, true); }

static const uint16_t *glyph(char c) {
  if (c < 32 || c > 126) c = '?';
  return font6x10[c - 32];
}

int gfx_char_width(int style) { return style == GFX_BIG ? 12 : style == GFX_BOLD ? 7 : 6; }

int gfx_text_width(const char *s, int style) {
  int n = (int)strlen(s);
  return n ? n * gfx_char_width(style) - (style == GFX_BIG ? 2 : 1) : 0;
}

static void draw_char(int x, int y, char c, int style) {
  const uint16_t *g = glyph(c);
  for (int col = 0; col < 5; col++) {
    for (int row = 0; row < FONT_ROWS; row++) {
      if (!(g[col] >> row & 1)) continue;
      if (style == GFX_BIG) {
        gfx_fill(x + 2 * col, y + 2 * row, 2, 2, true);
      } else {
        gfx_pixel(x + col, y + row, true);
        if (style == GFX_BOLD) gfx_pixel(x + col + 1, y + row, true);
      }
    }
  }
}

int gfx_text(int x, int y, const char *s, int style) {
  int cw = gfx_char_width(style);
  for (; *s; s++, x += cw) draw_char(x, y, *s, style);
  return x;
}

void gfx_text_center(int y, const char *s, int style) {
  gfx_text((GFX_W - gfx_text_width(s, style)) / 2, y, s, style);
}

static void icon_draw(int x, int y, const uint8_t icon[8], bool on) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (icon[r] & (0x80 >> c)) gfx_pixel(x + c, y + r, on);
}

void gfx_icon(int x, int y, const uint8_t icon[8]) { icon_draw(x, y, icon, true); }

void gfx_icon_inv(int x, int y, const uint8_t icon[8]) { icon_draw(x, y, icon, false); }

void gfx_round_fill(int x, int y, int w, int h) {
  gfx_fill(x, y, w, h, true);
  gfx_pixel(x, y, false);
  gfx_pixel(x + w - 1, y, false);
  gfx_pixel(x, y + h - 1, false);
  gfx_pixel(x + w - 1, y + h - 1, false);
}

void gfx_icon_text_center(int y, const uint8_t *icon, const char *s, int style) {
  int w = gfx_text_width(s, style) + (icon ? 8 + 4 : 0);
  int x = (GFX_W - w) / 2;
  if (icon) {
    // иконка 8 px по высоте, текст 7 px (14 у крупного) — выравниваем по центру
    gfx_icon(x, y + (style == GFX_BIG ? 3 : 0), icon);
    x += 12;
  }
  gfx_text(x, y, s, style);
}
