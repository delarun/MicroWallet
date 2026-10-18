// gfx.h — рисование в кадровый буфер 128x32 или 128x64 (без привязки к железу)
//
// Буфер в формате SSD1306: страницы по 8 строк, в байте столбец из 8 пикселей,
// младший бит сверху. Вывод на экран — oled_show() (или симулятор на ПК).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GFX_W 128
#ifndef GFX_H
#define GFX_H 64  // экран SSD1306 128x64 (32 — для экранов 128x32)
#endif

enum {
  GFX_NORMAL = 0,  // 5x9 (X11 6x10: 7 точек до базовой линии + 2 на хвосты), шаг 6 px
  GFX_BOLD = 1,    // 6x9, шаг 7 px
  GFX_BIG = 2,     // 10x18 (буквы 14, хвосты 4), шаг 12 px
};

extern uint8_t gfx_fb[GFX_W * GFX_H / 8];

void gfx_clear(void);
void gfx_pixel(int x, int y, bool on);
void gfx_fill(int x, int y, int w, int h, bool on);
void gfx_hline(int x, int y, int w);

int gfx_char_width(int style);
int gfx_text_width(const char *s, int style);
int gfx_text(int x, int y, const char *s, int style);  // возвращает x после текста
void gfx_text_center(int y, const char *s, int style);

// Иконка 8x8: 8 байт-строк, старший бит слева
void gfx_icon(int x, int y, const uint8_t icon[8]);
// То же, но гасит пиксели (для иконки на светлой плашке)
void gfx_icon_inv(int x, int y, const uint8_t icon[8]);
// Залитый прямоугольник со скруглёнными (срезанными) углами
void gfx_round_fill(int x, int y, int w, int h);
// Иконка + текст по центру экрана
void gfx_icon_text_center(int y, const uint8_t *icon, const char *s, int style);
