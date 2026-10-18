// oled.h — SSD1306 128x32 или 128x64 (GFX_H) по I2C, адрес 0x3C; пины — в hal
// Рисование — в gfx.h, здесь только вывод на железо.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void oled_init(void);
void oled_show(void);                 // вывести gfx_fb (и включить экран, если погашен)
void oled_contrast(uint8_t level);    // 0..255
void oled_com_pins(uint8_t v);        // раскладка COM (диагностика экрана)
void oled_all_on(bool on);            // зажечь все пиксели (проверка панели)
void oled_sleep(void);                // погасить экран до следующего oled_show()
bool oled_is_asleep(void);
void oled_hold_progress(int percent); // линия внизу при удержании кнопки; <0 = убрать
