// app.h — ядро кошелька; плата вызывает его из своего главного цикла
#pragma once
#include <stdbool.h>

#define APP_LINE_MAX 3072  // самая длинная строка команды (sign с 16 входами)

// Ответ одной строкой, без перевода строки: транспорт добавит свой разделитель
typedef void (*app_reply_fn)(const char *line);

void app_init(void);                          // экран, кнопка, разблокировка
void app_line(char *line, app_reply_fn out);  // выполнить команду
// Команды платформы (ESP32: ota ...): true — строка обработана, ответ отправлен
typedef bool (*app_ext_fn)(char *line, app_reply_fn out);
void app_set_ext(app_ext_fn fn);
// Экран занят долгой операцией (обновление прошивки): меню не перерисовывается,
// кнопка не листает меню, экран не гаснет и устройство не выключается
void app_busy(bool on);
bool app_is_busy(void);  // экран и кнопка сейчас у обновления или подтверждения
void app_tick(void);                          // кнопка и меню, звать каждые ~5 мс
// Аккумулятор почти пуст (и нет USB): тяжёлое (запись seed, прошивки, приложений) не начинать.
// Сам показывает предупреждение и возвращает true.
bool app_battery_low(void);
void app_redraw(void);                        // перерисовать главный экран

// Экран сопряжения по BLE: код и подтверждение кнопкой (блокирующий)
bool app_pair_confirm(const char *code);

// PIN (src/pin.c). После «Accept» подписи и установки прошивки или приложения: true — PIN
// верный или не задан. Неверный тратит попытку; после десятой кошелёк стирается.
bool app_pin_confirm(void);
bool app_locked(void);    // ждёт PIN после включения: команды, кроме ping и info, — «err locked»
void app_lock_redraw(void);  // вернуть экран PIN (после сопряжения и других всплывающих экранов)
void app_pin_setup(void);   // Settings: поставить или сменить PIN
void app_pin_remove(void);  // Settings: убрать PIN
