// touch_axs5106l.c — сенсор экрана Waveshare ESP32-S3-Touch-LCD-1.47: AXS5106L по I2C.
// hal_touch() отдаёт точку в долях экрана, повёрнутого так же, как в lcd_jd9853.c.
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"

#define TP_ADDR 0x63
#define PIN_SDA 42
#define PIN_SCL 41
// Пока палец на экране, INT опускается лишь короткими импульсами (по логу — в 70 замерах из
// 1273), а координаты в регистрах есть всё время. Поэтому с пальцем на экране читаем сенсор
// постоянно, без касания — по импульсу INT или раз в IDLE_US; палец убран — когда сенсор
// сам скажет «0 касаний» (или перестал отвечать дольше LOST_US).
#define TOUCH_US 4000
#define IDLE_US 20000
#define LOST_US 100000

static i2c_master_dev_handle_t tp;
static bool on;
static int px, py;
static uint64_t read_at, data_at;

void touch_init(void) {
  i2c_master_bus_config_t bus = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                 .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                 .flags.enable_internal_pullup = true};
  i2c_master_bus_handle_t h;
  if (i2c_new_master_bus(&bus, &h) != ESP_OK) return;
  i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR,
                             .scl_speed_hz = 400000};
  if (i2c_master_bus_add_device(h, &dev, &tp) != ESP_OK) return;
  gpio_config_t in = {.pin_bit_mask = 1ULL << TP_INT_GPIO, .mode = GPIO_MODE_INPUT,
                      .pull_up_en = GPIO_PULLUP_ENABLE};
  gpio_config(&in);
  // gpio_config, не gpio_set_direction: по умолчанию на GPIO47 не GPIO, а SPICLK_P
  gpio_config_t rst = {.pin_bit_mask = 1ULL << TP_RST_GPIO, .mode = GPIO_MODE_OUTPUT};
  gpio_config(&rst);
  gpio_sleep_sel_dis(TP_RST_GPIO);  // в лёгком сне сброс не отпускать (см. lcd_jd9853.c)
  gpio_set_level(TP_RST_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(TP_RST_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(150));
  gpio_wakeup_enable(TP_INT_GPIO, GPIO_INTR_LOW_LEVEL);  // касание будит из лёгкого сна
}

// Портретные координаты сенсора (x 0..171 зеркально к экрану, y 0..319) -> горизонтальный экран
// 320x172, повёрнутый так же, как в lcd_jd9853.c, -> тысячные доли
static void to_screen(int xr, int yr, int *x, int *y) {
#if CONFIG_MW_LCD_FLIP
  int lx = 319 - yr, ly = 171 - xr;
#else
  int lx = yr, ly = xr;
#endif
  *x = lx * 1000 / 320;
  *y = ly * 1000 / 172;
}

bool hal_touch(int *x, int *y) {
  if (!tp) return false;
  uint64_t now = hal_time_us(), since = now - read_at;
  if (since >= TOUCH_US && (on || since >= IDLE_US || gpio_get_level(TP_INT_GPIO) == 0)) {
    uint8_t r = 0x01, d[14] = {0};
    read_at = now;
    // запись регистра и чтение — отдельными транзакциями: повторный старт чип не принимает
    if (i2c_master_transmit(tp, &r, 1, 20) == ESP_OK && i2c_master_receive(tp, d, sizeof d, 20) == ESP_OK) {
      data_at = now;
      on = (d[1] & 0x0F) != 0;
      if (on) to_screen(((d[2] & 0x0F) << 8) | d[3], ((d[4] & 0x0F) << 8) | d[5], &px, &py);
    }
  }
  if (on && now - data_at > LOST_US) on = false;
  *x = px;
  *y = py;
  return on;
}
