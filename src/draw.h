// draw.h — рисование в цветной кадр 320x172 (RGB565) для цветного интерфейса (ui_lcd.c).
// Вывод на экран — oled_show() драйвера платы (esp32/main/lcd_jd9853.c) или предпросмотр на ПК.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LCD_W 320
#define LCD_H 172

#define RGB(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))

extern uint16_t *lcd_fb;  // LCD_W * LCD_H, строки сверху вниз

// Переход к следующему кадру (ui_anim): драйвер экрана показывает промежуточные кадры
enum { ANIM_NONE, ANIM_NEXT, ANIM_PREV, ANIM_FADE };
extern int lcd_anim;
// Что в кадре двигать при листании: столбцы [slide_x0, slide_x1) — «окно» карусели между
// стрелками; строки [fixed_y0, fixed_y1) (точки внизу) не едут, а сразу меняются.
// d_clear() сбрасывает: весь экран едет целиком.
typedef struct {
  int16_t slide_x0, slide_x1, fixed_y0, fixed_y1;
} anim_area_t;
extern anim_area_t lcd_anim_area;

typedef struct {
  uint16_t off;   // начало в bits
  uint8_t w, h;   // размер картинки глифа
  int8_t x, y;    // сдвиг картинки от позиции пера и от верха строки
  uint8_t adv;    // шаг пера
} glyph_t;

typedef struct {
  uint8_t height, ascent;  // высота строки и до базовой линии
  const glyph_t *g;        // символы 32..126
  const uint8_t *bits;     // 4 бита на точку (прозрачность), две точки в байте
} font_t;

extern const font_t font_small, font_body, font_title, font_big, font_mono, font_mono_s;  // fonts_lcd.c

bool draw_init(void);  // выделить кадр (PSRAM на ESP32)
void d_clear(uint16_t c);
void d_fill(int x, int y, int w, int h, uint16_t c);
void d_rrect(int x, int y, int w, int h, int r, uint16_t c);  // скруглённые углы со сглаживанием
void d_circle(int cx, int cy, int r, uint16_t c);             // круг со сглаживанием
uint16_t d_mix(uint16_t fg, uint16_t bg, int a);               // a = 0..255 (доля fg)

int d_text(int x, int y, const char *s, const font_t *f, uint16_t c);  // y — верх строки; -> x после
int d_text_n(int x, int y, const char *s, int n, const font_t *f, uint16_t c);
int d_text_w(const char *s, const font_t *f);
int d_text_wn(const char *s, int n, const font_t *f);
void d_text_c(int cx, int y, const char *s, const font_t *f, uint16_t c);  // по центру cx

void d_icon(int x, int y, const uint8_t icon[8], int scale, uint16_t c);  // иконка 8x8, увеличенная
void d_icon_c(int cx, int cy, const uint8_t icon[8], int scale, uint16_t c);  // центр рисунка — в (cx, cy)
void d_icon_sz(int cx, int cy, const uint8_t icon[8], float size, uint16_t c);  // клетка 8x8 -> size точек

// Промежуточный кадр перехода from -> to (t = 0..256) в строках y0..y1-1: NEXT — старый уезжает
// влево, новый въезжает справа; PREV — наоборот; FADE — плавная смена
void d_anim_frame(const uint16_t *from, const uint16_t *to, int kind, int t, int y0, int y1, uint16_t *out);
// Строки, где кадры различаются: [*y0, *y1); false — кадры одинаковые
bool d_diff_rows(const uint16_t *a, const uint16_t *b, int *y0, int *y1);
