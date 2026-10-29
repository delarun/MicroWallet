// hal_esp.c — ESP32-S3 SuperMini или Waveshare 1.47 (пины — menuconfig → MicroWallet)
//   экран SDA=GPIO8 SCL=GPIO9; кнопка GPIO1 -> GND (будит из сна), BOOT (GPIO0)
//   параллельно; аккумулятор: делитель VBUS/2 на плате (PR1/PR2) -> GPIO3
//   Waveshare 1.47: экран JD9853 по SPI (lcd_jd9853.c), кнопка BOOT (GPIO0, будит из сна),
//   аккумулятор: делитель BAT/3 -> GPIO12 (АЦП2)
//   ГСЧ: аппаратный TRNG. Он «истинный», когда работает радио (BLE);
//   если BLE выключен, на время генерации seed включаем внутренний источник шума.
#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "board.h"
#include "bootloader_random.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_flash_encrypt.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"
#include "soc/rtc_cntl_reg.h"

#define OLED_ADDR 0x3C

static i2c_master_dev_handle_t oled;

uint64_t hal_time_us(void) { return (uint64_t)esp_timer_get_time(); }

void hal_sleep_ms(uint32_t ms) { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1); }

void hal_random(uint8_t *buf, size_t len) { esp_fill_random(buf, len); }

void hal_entropy(uint8_t out[32]) {
  bool rf = ble_running();
  if (!rf) bootloader_random_enable();
  esp_fill_random(out, 32);
  if (!rf) bootloader_random_disable();
}


static void btn_from_rtc(void);

void hal_btn_init(void) {
  btn_from_rtc();
  gpio_config_t c = {
      .pin_bit_mask = 1ULL << CONFIG_MW_BTN_GPIO | 1ULL << BOOT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&c);
#if CONFIG_MW_BOARD_WS147
  touch_init();
#endif
}

#if !CONFIG_MW_BOARD_WS147
bool hal_touch(int *x, int *y) {  // сенсора нет; у Waveshare — touch_axs5106l.c
  (void)x;
  (void)y;
  return false;
}
#endif

// Внешняя кнопка и BOOT — одна и та же кнопка
bool hal_btn_raw(void) {
  return gpio_get_level(CONFIG_MW_BTN_GPIO) == 0 || gpio_get_level(BOOT_GPIO) == 0;
}

void hal_i2c_init(void) {
  i2c_master_bus_config_t bus = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = CONFIG_MW_OLED_SDA,
      .scl_io_num = CONFIG_MW_OLED_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t h;
  if (i2c_new_master_bus(&bus, &h) != ESP_OK) return;
  i2c_device_config_t dev = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = OLED_ADDR,
      .scl_speed_hz = CONFIG_MW_OLED_I2C_HZ,
  };
  i2c_master_bus_add_device(h, &dev, &oled);
}

bool hal_i2c_write(const uint8_t *buf, size_t len) {
  return oled && i2c_master_transmit(oled, buf, len, 50) == ESP_OK;
}

// Перезагрузка в загрузчик ROM: дальше idf.py/esptool прошивают без кнопок
void hal_reboot_bootloader(void) {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

// Для какой платы образ: телефон и ПК находят метку в *-app.bin и не дают поставить прошивку
// другой платы (чип в заголовке у обеих один — esp32s3). Нет метки (до v0.9) — SuperMini.
// hw= в info берётся из неё же: иначе компоновщик выбросит неиспользуемую строку.
static const char k_board_tag[] = "mw-board:" CONFIG_MW_HW_NAME ";";

const char *hal_platform(void) {  // "esp32s3", "esp32s3-ws147"
  static char hw[sizeof k_board_tag];
  if (!hw[0]) snprintf(hw, sizeof hw, "%.*s", (int)(sizeof k_board_tag - 11), k_board_tag + 9);
  return hw;
}

const char *hal_fw_build(void) {
  static char id[9];
  if (!id[0]) esp_app_get_elf_sha256(id, sizeof id);  // 8 hex-символов
  return id;
}

bool hal_usb_connected(void) { return usb_serial_jtag_is_connected(); }
bool hal_secure(void) { return esp_secure_boot_enabled() && esp_flash_encryption_enabled(); }

const char *hal_ota_info(void) {
  static char r[32];
  const esp_partition_t *run = esp_ota_get_running_partition(), *other = esp_ota_get_next_update_partition(NULL);
  esp_ota_img_states_t st;
  const char *s = "empty";
  if (other && esp_ota_get_state_partition(other, &st) == ESP_OK) {
    s = st == ESP_OTA_IMG_NEW              ? "new"
        : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
        : st == ESP_OTA_IMG_VALID          ? "valid"
        : st == ESP_OTA_IMG_INVALID        ? "invalid"
        : st == ESP_OTA_IMG_ABORTED        ? "aborted"
                                           : "undefined";
  }
  snprintf(r, sizeof r, "%s/%s", run ? run->label : "?", s);
  return r;
}
bool hal_ble_available(void) { return true; }
bool hal_ble_enabled(void) { return ble_enabled(); }
bool hal_ble_connected(void) { return ble_connected(); }
void hal_ble_set_enabled(bool on) { ble_set_enabled(on); }

// ---------- аккумулятор ----------
// На плате делитель VBUS/2 на GPIO3 (сигнал USB_IN). От USB там ~5 В — идёт зарядка,
// напряжение аккумулятора не измерить; от аккумулятора VBUS = BAT минус падение на
// диоде (~0.35 В).

static bool charging;

bool hal_charging(void) { return charging; }

int hal_battery_mv(void) {
#if CONFIG_MW_BATT_ENABLE
  static adc_oneshot_unit_handle_t adc;
  static adc_cali_handle_t cali;
  static adc_channel_t ch;
  static bool ready, failed;
  if (failed) return -1;
  if (!ready) {
    // АЦП1 (SuperMini, GPIO3) или АЦП2 (Waveshare 1.47, GPIO12): без Wi-Fi АЦП2 свободен
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_MW_BATT_GPIO, &unit, &ch) != ESP_OK) {
      failed = true;
      return -1;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_cali_curve_fitting_config_t kcfg = {
        .unit_id = unit, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, ch, &ccfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&kcfg, &cali) != ESP_OK) {
      failed = true;
      return -1;
    }
    ready = true;
  }
  int sum = 0, mv;
  for (int i = 0; i < 16; i++) {
    if (adc_oneshot_get_calibrated_result(adc, cali, ch, &mv) != ESP_OK) return -1;
    sum += mv;
  }
  mv = sum / 16 * CONFIG_MW_BATT_DIVIDER_X100 / 100;
  // Подключение к компьютеру по USB тоже значит внешнее питание
  charging = mv > CONFIG_MW_BATT_USB_MV || usb_serial_jtag_is_connected();
  if (charging) return -1;
  mv += CONFIG_MW_BATT_OFFSET_MV;
  return mv < 2500 ? -1 : mv;  // меньше 2.5 В — аккумулятора (или делителя) нет
#else
  return -1;
#endif
}

// ---------- энергосбережение ----------
// Лёгкий сон делает сам ESP-IDF (esp_pm + tickless idle), когда все задачи
// ждут. Здесь — только реже просыпаться в главном цикле и реже рекламировать BLE.

static volatile bool power_save;

bool hal_power_saving(void) { return power_save; }

void hal_power_save(bool on) {
  power_save = on;
  ble_set_slow(on);
}

// Глубокий сон: выключено почти всё (единицы-десятки микроампер), пробуждение —
// кнопкой и полная перезагрузка. EXT1 по RTC-пину (GPIO0..21); подтяжку RTC
// включаем сами и оставляем питание RTC-периферии, иначе во сне она пропадёт.
bool hal_power_off_available(void) { return esp_sleep_is_valid_wakeup_gpio(CONFIG_MW_BTN_GPIO); }

void hal_power_off(void) {
  if (!hal_power_off_available()) return;
  // Пробуждение — это новая загрузка: неподтверждённую прошивку загрузчик бы откатил
  esp_ota_mark_app_valid_cancel_rollback();
  // Автоматический лёгкий сон (esp_pm) оставляет заведённым таймер и GPIO-пробуждение —
  // с ними глубокий сон заканчивался сам через несколько секунд (wake:4 = TIMER).
  // Будить должна только кнопка.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_init(CONFIG_MW_BTN_GPIO);
  rtc_gpio_set_direction(CONFIG_MW_BTN_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(CONFIG_MW_BTN_GPIO);
  rtc_gpio_pulldown_dis(CONFIG_MW_BTN_GPIO);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup_io(1ULL << CONFIG_MW_BTN_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
#if CONFIG_MW_BOARD_WS147
  // Подсветка экрана (GPIO46): во сне ШИМ останавливается — держим пин в нуле;
  // сенсор — в сбросе (GPIO47 в нуле), так он почти не потребляет
  gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(LCD_BL_GPIO, 0);
  gpio_hold_en(LCD_BL_GPIO);
  gpio_set_level(TP_RST_GPIO, 0);
  gpio_hold_en(TP_RST_GPIO);
  gpio_deep_sleep_hold_en();
#endif
  esp_deep_sleep_start();
}

// Этап загрузки — в RTC-памяти: она переживает сброс от просадки или паники, и после
// него видно, на каком шаге он случился (boot=brown@ble). Шаги — hal_boot_stage() в main.c.
#define STAGE_MAGIC 0x5a6e0000u
static RTC_NOINIT_ATTR uint32_t s_stage;
static int s_prev_stage = -1;
// шаг, который выполнялся в момент сбоя
static const char *const k_stages[] = {"nvs", "pm", "usb", "ble", "init", "run"};

void hal_boot_stage(int n) {
  if (s_prev_stage < 0)
    s_prev_stage = (s_stage & 0xffff0000u) == STAGE_MAGIC ? (int)(s_stage & 0xff) : 0xff;
  s_stage = STAGE_MAGIC | (uint32_t)n;
}

static const char *boot_reason_base(char *r, size_t n) {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return strcpy(r, "pwr");
    case ESP_RST_EXT: return strcpy(r, "ext");
    case ESP_RST_SW: return strcpy(r, "sw");
    case ESP_RST_PANIC: return strcpy(r, "panic");
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return strcpy(r, "wdt");
    case ESP_RST_BROWNOUT: return strcpy(r, "brown");
    case ESP_RST_USB: return strcpy(r, "usb");
    case ESP_RST_DEEPSLEEP: {
      esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
      if (c == ESP_SLEEP_WAKEUP_EXT1)
        snprintf(r, n, "btn:%llx", (unsigned long long)esp_sleep_get_ext1_wakeup_status());
      else snprintf(r, n, "wake:%d", (int)c);
      return r;
    }
    default: snprintf(r, n, "rst:%d", (int)esp_reset_reason()); return r;
  }
}

// Для диагностики «само включилось»: сброс или пробуждение, и от чего именно
const char *hal_boot_reason(void) {
  static char r[24];
  if (r[0]) return r;
  boot_reason_base(r, sizeof r);
  esp_reset_reason_t rr = esp_reset_reason();
  bool fault = rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
               rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
  int st = s_prev_stage;
  if (fault && st >= 0 && st < (int)(sizeof k_stages / sizeof *k_stages))
    snprintf(r + strlen(r), sizeof r - strlen(r), "@%s", k_stages[st]);
  return r;
}

void hal_heap(uint32_t *f, uint32_t *l) {
  *f = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  *l = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

bool hal_resumed(void) {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  return c == ESP_SLEEP_WAKEUP_EXT1;
}

// После пробуждения пин ещё в режиме RTC — вернуть его обычному GPIO
static void btn_from_rtc(void) {
  if (rtc_gpio_is_valid_gpio(CONFIG_MW_BTN_GPIO)) rtc_gpio_deinit(CONFIG_MW_BTN_GPIO);
#if CONFIG_MW_BOARD_WS147
  gpio_hold_dis(LCD_BL_GPIO);
  gpio_hold_dis(TP_RST_GPIO);
#endif
}

// ---------- приложения: раздел "apps" ----------
// Раздела нет у плат со старой таблицей разделов: тогда приложений просто нет.

static const esp_partition_t *apps_part(void) {
  static const esp_partition_t *p;
  static bool looked;
  if (!looked) {
    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "apps");
    looked = true;
  }
  return p;
}

uint32_t hal_apps_size(void) { return apps_part() ? apps_part()->size : 0; }

bool hal_apps_read(uint32_t off, void *buf, uint32_t len) {
  const esp_partition_t *p = apps_part();
  return p && off + len <= p->size && esp_partition_read(p, off, buf, len) == ESP_OK;
}

bool hal_apps_write(uint32_t off, const void *data, uint32_t len) {
  const esp_partition_t *p = apps_part();
  return p && off + len <= p->size && esp_partition_write(p, off, data, len) == ESP_OK;
}

bool hal_apps_erase(uint32_t off, uint32_t len) {
  const esp_partition_t *p = apps_part();
  return p && off + len <= p->size && esp_partition_erase_range(p, off, len) == ESP_OK;
}
