// menu.h — главное меню: карусель монет -> Receive / Info / Back, настройки.
// Не блокирует: main() зовёт menu_event() на каждое нажатие и продолжает
// обслуживать USB, пока пользователь ходит по меню.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "button.h"

void menu_init(void);
void menu_reset(void);          // на главный экран
void menu_draw(void);           // нарисовать текущий экран и показать
bool menu_on_dashboard(void);   // сейчас главный экран (там строка статуса)
void menu_event(btn_t b);
uint32_t menu_screen_off_ms(void);  // через сколько гасить экран и спать (0 = никогда)
uint32_t menu_power_off_ms(void);   // через сколько выключаться (глубокий сон; 0 = никогда)
int menu_addr_count(void);          // адресов для получения: 1, 5, 10 или 20
bool menu_set_addr_count(int n);    // false — не из этого списка
int menu_battery_pct(void);         // -1 — аккумулятора нет
int menu_battery_mv(void);

// Реализуются в app.c
bool app_create_wallet(void);
void app_wipe(void);
void app_power_off(void);
