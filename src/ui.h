// ui.h — макеты экранов в стиле карусели (только рисование, без кнопки)
//
// Навигация везде одинаковая: короткое нажатие — следующий экран,
// удержание — выбрать. Справа «>», внизу точки — где ты в списке.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define UI_LINE 21              // символов в строке мелким шрифтом

// Главный экран: все приложения одной строкой иконок (как на Ledger Nano X),
// выбранная — на светлой плашке, под строкой название и подпись
void ui_dashboard(const uint8_t *const icons[], int count, int sel, const char *title,
                  const char *sub);
// Строка статуса над главным экраном (только на экране 128x64)
// batt_pct: -1 — аккумулятора нет, UI_BATT_CHARGING — питание от USB, идёт зарядка
#define UI_BATT_CHARGING (-2)
void ui_status(const char *left, bool ble_on, bool ble_connected, bool usb, int batt_pct);
// Пункт меню: иконка + жирный заголовок, подпись, точки idx/count
void ui_carousel(const uint8_t *icon, const char *title, const char *sub, int idx, int count);
// Сообщение по центру (без навигации)
void ui_message(const uint8_t *icon, const char *title, const char *sub);
// Поле: заголовок слева, "page/pages" справа, значение в две строки
void ui_field(const uint8_t *icon, const char *title, const char *value,
              int page, int pages, int idx, int count);
// Сумма: крупно, если влезает, иначе как поле
void ui_amount(const uint8_t *icon, const char *title, const char *value, int idx, int count);
void ui_progress(const uint8_t *icon, const char *title, int percent);
// Слово мнемоники крупно
void ui_pin(const char *title, int entered, int level, bool confirm);  // ввод PIN (pin.c)
void ui_word(const char *title, const char *word, int idx, int count);
void ui_splash(const char *version, const char *net);

int ui_pages(const char *value);  // на сколько страниц разобьётся значение
int ui_battery_pct(int mv);        // мВ Li-ion -> 0..100 (-1, если mv < 0)
