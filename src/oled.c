#include "oled.h"

#include <string.h>

#include "gfx.h"
#include "hal.h"

#define W GFX_W
#define PAGES (GFX_H / 8)

#ifndef OLED_COM_PINS  // 0x12 — почти все 128x64, 0x02 — 128x32
#define OLED_COM_PINS (GFX_H == 64 ? 0x12 : 0x02)
#endif

static uint8_t saved_row[W];
static bool hold_drawn, asleep;

// Команда с параметрами — одной транзакцией. Если шина сбойнула, повторяем:
// длинные провода и слабые подтяжки иначе дают полосы и потерянные байты.
static bool cmds(const uint8_t *c, size_t n) {
  uint8_t b[8] = {0x00};
  memcpy(b + 1, c, n);
  for (int t = 0; t < 3; t++)
    if (hal_i2c_write(b, n + 1)) return true;
  return false;
}

static void cmd(uint8_t c) { cmds(&c, 1); }

void oled_init(void) {
  hal_i2c_init();
  static const uint8_t init[] = {
      0xAE,        // display off
      0xD5, 0x80,  // clock
      0xA8, GFX_H - 1,  // multiplex = число строк
      0xD3, 0x00,  // offset
      0x40,        // start line 0
      0x8D, 0x14,  // charge pump on
      0x20, 0x00,  // горизонтальная адресация
      0xA1, 0xC8,  // ориентация (если перевёрнуто: 0xA0, 0xC0)
      0xDA, OLED_COM_PINS,  // раскладка COM: 0x12 для 128x64, 0x02 для 128x32
      0x81, 0x8F,  // контраст
      0xD9, 0xF1,  // precharge
      0xDB, 0x40,  // VCOMH
      0xA4, 0xA6,  // из RAM, не инверсия
      0xAF,        // display on
  };
  for (size_t i = 0; i < sizeof init; i++) cmd(init[i]);
  gfx_clear();
  oled_show();
}

// Страница (8 строк) целиком: свой адрес + 128 байт одной транзакцией.
// При ошибке страница отправляется заново с тем же адресом, сбой не
// сдвигает остальную картинку.
static void show_page(int p) {
  static uint8_t b[W + 1];
  const uint8_t addr[6] = {0x21, 0, W - 1, 0x22, (uint8_t)p, (uint8_t)p};
  b[0] = 0x40;
  memcpy(b + 1, gfx_fb + p * W, W);
  for (int t = 0; t < 3; t++)
    if (cmds(addr, sizeof addr) && hal_i2c_write(b, sizeof b)) return;
}

void oled_show(void) {
  if (asleep) {
    cmd(0xAF);
    asleep = false;
  }
  for (int p = 0; p < PAGES; p++) show_page(p);
}

// Диагностика: все пиксели горят независимо от памяти (0xA5) или обычный режим (0xA4)
void oled_all_on(bool on) { cmd(on ? 0xA5 : 0xA4); }

void oled_com_pins(uint8_t v) {
  cmd(0xDA);
  cmd(v);
}

void oled_contrast(uint8_t level) {
  cmd(0x81);
  cmd(level);
}

void oled_sleep(void) {
  cmd(0xAE);
  asleep = true;
}

bool oled_is_asleep(void) { return asleep; }

// Линия вдоль нижнего края, пока держишь кнопку (не портит то, что под ней)
void oled_hold_progress(int percent) {
  uint8_t *last = gfx_fb + (PAGES - 1) * W;
  if (asleep) oled_show();  // разбудить экран целиком
  if (percent < 0) {
    if (hold_drawn) memcpy(last, saved_row, W);
    hold_drawn = false;
    show_page(PAGES - 1);
    return;
  }
  if (!hold_drawn) memcpy(saved_row, last, W);
  hold_drawn = true;
  int n = W * percent / 100;
  for (int x = 0; x < W; x++) last[x] = x < n ? (saved_row[x] | 0x80) : saved_row[x];
  show_page(PAGES - 1);  // меняется только нижняя страница
}
