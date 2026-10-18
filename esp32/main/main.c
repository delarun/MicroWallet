// MicroWallet на ESP32-S3: ядро src/app.c + USB Serial/JTAG + BLE (шифрованный канал).
//
// Все команды выполняются в одной задаче (app_main) по очереди: строки из USB
// и кадры из BLE приходят через очередь, ответ уходит туда же, откуда пришёл запрос.
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ble.h"
#include "board.h"
#include "channel.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal.h"
#include "memzero.h"
#include "nvs_flash.h"
#include "ota.h"

bool hal_power_saving(void);  // hal_esp.c
void hal_boot_stage(int n);  // hal_esp.c: шаг загрузки, виден в info после сбоя (boot=brown@ble)

#if CONFIG_PM_ENABLE
// USB Serial/JTAG в лёгком сне не работает, а штатная защита ESP-IDF
// включается только при консоли на USB. Поэтому сами не даём спать, пока
// компьютер на связи (драйвер видит пакеты SOF от хоста).
static esp_pm_lock_handle_t usb_lock;
static bool usb_locked;

static void usb_pm_update(void) {
  // Драйвер считает USB подключённым по свежим пакетам SOF. Во время записи во флеш
  // прерывания выключены, и SOF теряются — держим «на связи» ещё 3 с после последнего.
  static uint64_t seen;
  uint64_t now = hal_time_us();
  if (usb_serial_jtag_is_connected()) seen = now;
  bool c = seen && now - seen < 3000000;
  if (c == usb_locked) return;
  if (c) esp_pm_lock_acquire(usb_lock);
  else esp_pm_lock_release(usb_lock);
  usb_locked = c;
}
#endif

typedef struct {
  int src;
  size_t len;
  uint8_t *data;
} msg_t;

static QueueHandle_t q;

void mw_post(int src, const uint8_t *data, size_t len) {
  msg_t m = {.src = src, .len = len};
  if (len) {
    m.data = malloc(len + 1);
    if (!m.data) return;
    memcpy(m.data, data, len);
    m.data[len] = 0;
  }
  if (xQueueSend(q, &m, 0) != pdTRUE && m.data) {
    memzero(m.data, len);
    free(m.data);
  }
}

static void usb_reply(const char *s) {
  usb_serial_jtag_write_bytes(s, strlen(s), portMAX_DELAY);
  usb_serial_jtag_write_bytes("\n", 1, portMAX_DELAY);
}

// Команды обновления прошивки: по USB и по шифрованному каналу BLE
static bool ext_line(char *line, app_reply_fn out) {
  return ota_line(line, out, out != usb_reply);
}

// Построчное чтение USB Serial/JTAG
static void usb_task(void *arg) {
  static char line[APP_LINE_MAX];
  size_t len = 0;
  bool overflow = false;
  uint8_t buf[64];
  for (;;) {
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
      char ch = (char)buf[i];
      if (ch == '\r') continue;
      if (ch == '\n') {
        if (overflow) usb_reply("err line too long");
        else mw_post(MW_SRC_USB, (const uint8_t *)line, len);
        memzero(line, len);
        len = 0;
        overflow = false;
      } else if (len < sizeof line - 1) {
        line[len++] = ch;
      } else {
        overflow = true;
      }
    }
  }
}

void app_main(void) {
  hal_boot_stage(0);
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Раздел NVS испорчен или от другой версии — без него кошелька всё равно нет
    nvs_flash_erase();
    nvs_flash_init();
  }
  hal_boot_stage(1);

#if CONFIG_PM_ENABLE
  // Автоматический лёгкий сон: чип спит, пока все задачи ждут. BLE держит
  // соединение (modem sleep), кнопка будит сразу. Пока к USB подключён
  // компьютер, драйвер USB сам не даёт засыпать.
  esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &usb_lock);
  esp_pm_lock_acquire(usb_lock);  // пока не знаем — считаем, что USB подключён
  usb_locked = true;
#ifdef CONFIG_MW_LIGHT_SLEEP
  const bool light_sleep = true;
#else
  const bool light_sleep = false;  // только снижение частоты
#endif
  esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = light_sleep};
  esp_pm_configure(&pm);
  gpio_wakeup_enable(CONFIG_MW_BTN_GPIO, GPIO_INTR_LOW_LEVEL);
  if (BOOT_GPIO != CONFIG_MW_BTN_GPIO)
    gpio_wakeup_enable(BOOT_GPIO, GPIO_INTR_LOW_LEVEL);  // BOOT тоже будит из лёгкого сна
  esp_sleep_enable_gpio_wakeup();
#endif

  hal_boot_stage(2);
  q = xQueueCreate(16, sizeof(msg_t));
  usb_serial_jtag_driver_config_t ucfg = {.rx_buffer_size = 4096, .tx_buffer_size = 4096};
  usb_serial_jtag_driver_install(&ucfg);
  xTaskCreate(usb_task, "usb", 4096, NULL, 5, NULL);
  hal_boot_stage(3);
  ble_init();
  hal_boot_stage(4);

  app_set_ext(ext_line);
  app_init();
  hal_boot_stage(5);
  for (;;) {
    // Новая прошивка годная, если проработала 30 с: упадёт раньше — загрузчик вернёт
    // прошлую (с Secure Boot иначе её не спасти). Перед выключением — hal_power_off.
    static bool valid;
    if (!valid && hal_time_us() > 30 * 1000000ULL) {
      esp_ota_mark_app_valid_cancel_rollback();
      valid = true;
    }
    msg_t m;
    while (xQueueReceive(q, &m, 0) == pdTRUE) {
      if (m.src == MW_SRC_USB) app_line((char *)m.data, usb_reply);
      else if (m.src == MW_SRC_BLE) chan_frame(m.data, m.len, ble_send_frame);
      else {
        chan_reset();
        ota_link_lost();  // соединение BLE закрыто — обновление по нему не закончится
      }
      if (m.data) {
        memzero(m.data, m.len);
        free(m.data);
      }
    }
    app_tick();
    ota_tick();
#if CONFIG_PM_ENABLE
    usb_pm_update();
#endif
    // Во сне просыпаемся реже: кнопка всё равно разбудит, команда — тоже
    vTaskDelay(pdMS_TO_TICKS(hal_power_saving() ? 100 : 5));
  }
}
