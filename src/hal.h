// hal.h — всё, что ядру нужно от конкретной платы. Реализации:
//   esp32/main/hal_esp.c     — ESP32-S3 SuperMini
//   tools/sim/hal_sim.c      — симулятор на ПК (для тестов)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t hal_time_us(void);
void hal_sleep_ms(uint32_t ms);

// Случайные байты для подписи и протокола (аппаратный ГСЧ, если есть)
void hal_random(uint8_t *buf, size_t len);
// 32 байта сырой энтропии для нового seed; ядро смешивает их через SHA-256
// с временем и длительностью нажатий
void hal_entropy(uint8_t out[32]);

void hal_btn_init(void);
bool hal_btn_raw(void);  // кнопка нажата

void hal_i2c_init(void);
bool hal_i2c_write(const uint8_t *buf, size_t len);  // на адрес экрана 0x3C; false — сбой шины

void hal_reboot_bootloader(void);  // режим прошивки по USB
const char *hal_platform(void);    // "esp32s3" | "sim"
bool hal_secure(void);             // Secure Boot и шифрование флеша включены (eFuse)
const char *hal_fw_build(void);    // id сборки (ESP32: начало SHA-256 ELF) или "-"

// Связь. В симуляторе BLE нет — заглушки возвращают false.
bool hal_usb_connected(void);
bool hal_ble_available(void);
bool hal_ble_enabled(void);
bool hal_ble_connected(void);
void hal_ble_set_enabled(bool on);
void hal_ble_forget_all(void);  // забыть все сопряжённые ПК

// Питание. В симуляторе аккумулятора нет: -1 и пустые заглушки.
int hal_battery_mv(void);         // напряжение аккумулятора, мВ; -1 — не подключён или не измерить
bool hal_charging(void);          // есть внешнее питание 5 В (по последнему замеру): идёт зарядка
void hal_power_save(bool on);     // простой: экран погашен, плата может засыпать
bool hal_power_off_available(void);  // есть ли глубокий сон с пробуждением кнопкой
void hal_power_off(void);            // глубокий сон; просыпается кнопкой (через перезагрузку)
bool hal_resumed(void);              // запуск — пробуждение из глубокого сна (без заставки)
const char *hal_boot_reason(void);
void hal_heap(uint32_t *free_bytes, uint32_t *largest_block);  // для info: хватит ли памяти приложениям   // почему запустились: pwr, btn, sw, panic, wdt, brown, … (для info)

// Приложения (WebAssembly): раздел "apps" во флеше, без фиксированных слотов (src/apps.c
// раскладывает их сам). Стирание — секторами. Нет раздела — hal_apps_size() == 0.
#define HAL_APPS_SECTOR 4096
uint32_t hal_apps_size(void);
bool hal_apps_read(uint32_t off, void *buf, uint32_t len);
bool hal_apps_write(uint32_t off, const void *data, uint32_t len);  // только в стёртое место
bool hal_apps_erase(uint32_t off, uint32_t len);                    // off и len кратны сектору
