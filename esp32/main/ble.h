// ble.h — BLE-периферия MicroWallet (NimBLE): сервис с RX (запись) и TX (уведомления).
// Принятые кадры уходят в главный цикл через mw_post(); отвечает он через ble_send_frame().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ble_init(void);
bool ble_running(void);    // контроллер включён (радио работает)
bool ble_enabled(void);    // разрешён пользователем (реклама/подключения)
bool ble_connected(void);
void ble_set_enabled(bool on);
void ble_send_frame(const uint8_t *frame, size_t len);
void ble_set_slow(bool slow);  // реже рекламироваться (энергосбережение)

// main.c: передать событие в главный цикл
enum { MW_SRC_USB, MW_SRC_BLE, MW_SRC_BLE_RESET };
void mw_post(int src, const uint8_t *data, size_t len);
