// board.h — особенности плат: ESP32-S3 SuperMini и Waveshare ESP32-S3-Touch-LCD-1.47
#pragma once

// Встроенная кнопка BOOT (GPIO0) — запасная кнопка кошелька (меню, лёгкий сон).
// На SuperMini из глубокого сна будит основная кнопка (CONFIG_MW_BTN_GPIO). У Waveshare
// другой кнопки нет: там основная — сама BOOT (выводы выбора режима загрузки читаются
// только при включении питания и сбросе, пробуждение из сна их не читает).
#define BOOT_GPIO 0

// Waveshare 1.47: подсветка экрана (во сне держим в нуле)
#define LCD_BL_GPIO 46
#define TP_RST_GPIO 47  // сенсор AXS5106L (I2C 0x63, SDA 42, SCL 41)
#define TP_INT_GPIO 48
void touch_init(void);  // touch_axs5106l.c
