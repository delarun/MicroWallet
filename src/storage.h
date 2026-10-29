// storage.h — хранение энтропии seed (16/24/32 байта) и настроек.
// Реализации: esp32/main/storage_esp.c (NVS), tools/sim/hal_sim.c (файлы).
// В обычной сборке энтропия лежит В ОТКРЫТОМ ВИДЕ.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool storage_load(uint8_t entropy[32], size_t *len);
bool storage_save(const uint8_t *entropy, size_t len);
void storage_wipe(void);  // seed и PIN

// PIN (src/pin.c): хеш с солью и счётчик неверных попыток подряд. Нет записи — PIN не задан.
typedef struct {
  uint8_t ver, fails, salt[16], hash[32];
} pin_rec_t;
bool storage_pin_load(pin_rec_t *r);
bool storage_pin_save(const pin_rec_t *r);
void storage_pin_clear(void);

// Настройки интерфейса — в отдельном секторе (перед сектором seed), чтобы
// их перезапись никогда не трогала seed.
typedef struct {
  uint8_t brightness;  // 0..3
  uint8_t screen_off;  // индекс в таблице таймаутов
  uint8_t ble_off;     // 1 = Bluetooth выключен пользователем
  uint8_t power_off;   // индекс в таблице выключения (глубокий сон)
  uint8_t addrs;       // индекс в таблице «сколько адресов для получения» (menu.c)
} settings_t;

// Значения по умолчанию. Старый блок (до поля addrs, SETTINGS_V1_SIZE байт) тоже
// читается: остальные настройки не сбрасываются при обновлении прошивки.
#define SETTINGS_DEFAULT ((settings_t){2, 1, 0, 0, 3})
#define SETTINGS_V1_SIZE 4

void settings_load(settings_t *s);  // при ошибке — значения по умолчанию
bool settings_save(const settings_t *s);

// Баланс монеты, присланный телефоном или ПК (команда bal), — чтобы видеть его на устройстве в
// меню монеты. when — местное время обновления (секунды с 1970, часы телефона), text — суммы
// через ';'. Стирается вместе с seed (storage_save, storage_wipe): у другого кошелька он другой.
#define BAL_TEXT_MAX 120
bool storage_balance_load(const char *id, uint32_t *when, char *text, size_t len);
bool storage_balance_save(const char *id, uint32_t when, const char *text);
void storage_balance_clear(void);

// Порядок приложений на главном экране: id через запятую. Нет — пустая строка.
void apps_order_load(char *out, size_t len);
bool apps_order_save(const char *s);
