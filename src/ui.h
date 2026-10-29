// ui.h — макеты экранов в стиле карусели (только рисование, без кнопки)
//
// Навигация везде одинаковая: короткое нажатие — следующий экран,
// удержание — выбрать. Справа «>», внизу точки — где ты в списке.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define UI_LINE 21              // символов в строке мелким шрифтом

// Главный экран: все приложения (icons, names — по одному на пункт, count — сколько).
// OLED: строка иконок (как на Ledger Nano X), выбранная — на светлой плашке; цветной экран:
// карусель кружков в цветах монет. Под ними название и подпись выбранного.
#define UI_DASH_MAX 72
void ui_dashboard(const uint8_t *const icons[], const char *const names[], int count, int sel,
                  const char *title, const char *sub);
// Карусель в дробном положении pos (по кругу: -1 — последний пункт, count — первый), по данным
// последнего ui_dashboard и ui_status: палец тянет ленту или она доезжает. false — экран так
// не умеет (OLED).
bool ui_dashboard_at(float pos);
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
// Баланс монеты из кэша (команда bal): суммы через ';' («0.5 ETH;120 USDT»), when — «29.10 23:40».
// OLED — поле текстом, цветной экран — список: символ слева, сумма справа
void ui_balance(const uint8_t *icon, const char *when, const char *text, int page, int pages);
int ui_balance_pages(const char *text);
// Адрес для получения: на OLED — как поле (по страницам), на цветном экране — QR-код и адрес
void ui_receive(const uint8_t *icon, const char *title, const char *addr, int page, int pages);
int ui_receive_pages(const char *addr);
// Сумма: крупно, если влезает, иначе как поле
void ui_amount(const uint8_t *icon, const char *title, const char *value, int idx, int count);
void ui_progress(const uint8_t *icon, const char *title, int percent);
// Слово мнемоники крупно
void ui_pin(const char *title, int entered, int level, bool confirm);  // ввод PIN (pin.c)
void ui_word(const char *title, const char *word, int idx, int count);
void ui_splash(const char *version, const char *net);

int ui_pages(const char *value);  // на сколько страниц разобьётся значение

// Как показать следующий кадр (цветной экран; на OLED — сразу): листание вперёд/назад
// или плавная смена. Действует на ближайший oled_show().
enum { UI_ANIM_NONE, UI_ANIM_NEXT, UI_ANIM_PREV, UI_ANIM_FADE };
void ui_anim(int kind);
void ui_anim_btn(int btn);  // по событию кнопки (button.h): дальше, назад или выбор
int ui_battery_pct(int mv);        // мВ Li-ion -> 0..100 (-1, если mv < 0)
