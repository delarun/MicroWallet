// lcd_jd9853.c — экран Waveshare ESP32-S3-Touch-LCD-1.47: JD9853 172x320 по SPI.
// Интерфейс oled.h для цветного кадра: интерфейс (src/ui_lcd.c) рисует в lcd_fb 320x172
// (draw.h), oled_show() выводит его на экран, повёрнутый горизонтально.
//
// Контроллер остаётся в портретной ориентации (MADCTL 0, как в демо Waveshare), поворот —
// программный, при выводе полосы. Горизонтальная точка (lx, ly) — это портретная
// (px, py) = (171 - ly, lx); с MW_LCD_FLIP — (ly, 319 - lx).
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "draw.h"
#include "oled.h"

#define COL_OFF 34  // панель 172 точки — середина памяти контроллера 240 (портретные столбцы)
#define STRIP 16    // портретных строк (= горизонтальных столбцов) за одну передачу
#define HOLD_H 4    // полоса удержания внизу

#define PIN_MOSI 39
#define PIN_SCLK 38
#define PIN_CS 21
#define PIN_DC 45
#define PIN_RST 40
#define PIN_BL 46

static esp_lcd_panel_io_handle_t io;
static uint16_t *bufs[2];  // полосы LCD_H x STRIP (портрет) по очереди; внутренняя память (DMA)
static int buf_busy;       // в какую полосу идёт (или шла последней) передача
static uint16_t saved[LCD_W * HOLD_H];
static uint16_t *shown, *tmp;  // что сейчас на экране и промежуточный кадр анимации (PSRAM)
static bool full_next;         // на экране не то, что в shown (диагностика): следующий кадр — целиком
static bool hold_drawn, asleep;
static uint32_t duty = 600;

// Пока экран горит — без лёгкого сна: во сне останавливается ШИМ подсветки, и экран мерцал
// при питании от зарядки или аккумулятора (от компьютера сна и так нет). Подсветка тратит
// десятки мА, сон процессора рядом с ней почти ничего не экономит; экран погас — спим.
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t awake_lock;
#endif

static void stay_awake(bool on) {
#if CONFIG_PM_ENABLE
  static bool held;
  if (!awake_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "lcd", &awake_lock) != ESP_OK) return;
  if (on == held) return;
  if (on) esp_pm_lock_acquire(awake_lock);
  else esp_pm_lock_release(awake_lock);
  held = on;
#else
  (void)on;
#endif
}

static void cmd(uint8_t c, const uint8_t *d, size_t n) { esp_lcd_panel_io_tx_param(io, c, d, n); }

// Подсветка меняется плавно (аппаратный fade LEDC): экран загорается и гаснет, а не мигает
static void backlight(uint32_t d, int ms, bool wait) {
  ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, d, ms, wait ? LEDC_FADE_WAIT_DONE : LEDC_FADE_NO_WAIT);
}

// Окно в портретных координатах. Каждую полосу — своим окном и RAMWR: «продолжить запись»
// (0x3C) этот контроллер ведёт неверно. tx_param ждёт конца предыдущей передачи цвета, так
// что буфер можно заполнять только после window().
static void window(int px, int py, int w, int h) {
  int x1 = px + COL_OFF + w - 1, y1 = py + h - 1;
  px += COL_OFF;
  uint8_t ca[4] = {(uint8_t)(px >> 8), (uint8_t)px, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t ra[4] = {(uint8_t)(py >> 8), (uint8_t)py, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A, ca, 4);
  cmd(0x2B, ra, 4);
}

// Вывести горизонтальные строки ly0..ly0+h-1 кадра fb: на экране это портретные столбцы,
// идём полосами по STRIP портретных строк (горизонтальных столбцов lx). Две полосы по очереди:
// пока одна уходит по DMA, вторая готовится — кадр успевает быстрее, чем экран обновляется
// сам (16.7 мс), и не рвётся.
static void fill_strip(uint16_t *o, const uint16_t *fb, int ly0, int h, int y) {
  for (int r = 0; r < STRIP; r++) {
#if CONFIG_MW_LCD_FLIP
    const uint16_t *src = fb + ly0 * LCD_W + (LCD_W - 1 - (y + r));
    for (int c = 0; c < h; c++, src += LCD_W) *o++ = (uint16_t)(*src >> 8 | *src << 8);
#else
    const uint16_t *src = fb + (ly0 + h - 1) * LCD_W + y + r;
    for (int c = 0; c < h; c++, src -= LCD_W) *o++ = (uint16_t)(*src >> 8 | *src << 8);
#endif
  }
}

static void flush_from(const uint16_t *fb, int ly0, int h) {
  int k = buf_busy ^ 1;  // свободная: прошлая передача могла ещё идти из другой
  fill_strip(bufs[k], fb, ly0, h, 0);
  for (int y = 0; y < LCD_W; y += STRIP, k ^= 1) {
#if CONFIG_MW_LCD_FLIP
    window(ly0, y, h, STRIP);  // ждёт конца передачи предыдущей полосы
#else
    window(LCD_H - ly0 - h, y, h, STRIP);
#endif
    esp_lcd_panel_io_tx_color(io, 0x2C, bufs[k], h * STRIP * 2);
    buf_busy = k;
    if (y + STRIP < LCD_W) fill_strip(bufs[k ^ 1], fb, ly0, h, y + STRIP);
  }
}

static void flush_rows(int ly0, int h) { flush_from(lcd_fb, ly0, h); }

static void remember(void) {
  if (shown) memcpy(shown, lcd_fb, LCD_W * LCD_H * 2);
}

static void wake(void) {
  if (!asleep) return;
  stay_awake(true);
  cmd(0x11, NULL, 0);  // выход из сна: 120 мс до следующих команд
  vTaskDelay(pdMS_TO_TICKS(120));
  cmd(0x29, NULL, 0);
  asleep = false;
  flush_rows(0, LCD_H);
  remember();
  backlight(duty, 200, false);  // после кадра: не мелькнуть старой картинкой
}

void oled_init(void) {
  if (!draw_init()) return;
  spi_bus_config_t bus = {.mosi_io_num = PIN_MOSI, .sclk_io_num = PIN_SCLK, .miso_io_num = -1,
                          .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_H * STRIP * 2};
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return;
  esp_lcd_panel_io_spi_config_t cfg = {.cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC, .spi_mode = 0,
                                       .pclk_hz = 80 * 1000 * 1000, .trans_queue_depth = 4,
                                       .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io) != ESP_OK) return;
  for (int i = 0; i < 2; i++) bufs[i] = heap_caps_malloc(LCD_H * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  shown = heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);  // нет памяти — без анимации
  tmp = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);

  // подсветка — ШИМ, пока выключена
  ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
                           .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
  ledc_timer_config(&t);
  ledc_channel_config_t ch = {.speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                              .timer_sel = LEDC_TIMER_0, .gpio_num = PIN_BL, .duty = 0};
  ledc_channel_config(&ch);
  ledc_fade_func_install(0);

  // Сброс — через gpio_config: он переключает мультиплексор выводов на GPIO. По умолчанию
  // на GPIO40 сигнал JTAG MTDO, и gpio_set_direction() вывод бы не трогал: сброс до экрана
  // не доходил, линия плавала, экран то запускался, то нет.
  gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT};
  gpio_config(&rst);
  gpio_set_level(PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(120));
  // В лёгком сне ESP-IDF изолирует выводы (CONFIG_PM_SLP_DISABLE_GPIO): сброс, CS и DC
  // повисли бы в воздухе — экран мог сброситься или принять мусор. Экрану — свои уровни и во сне.
  static const int keep[] = {PIN_RST, PIN_CS, PIN_DC, PIN_SCLK, PIN_MOSI, PIN_BL};
  for (size_t i = 0; i < sizeof keep / sizeof *keep; i++) gpio_sleep_sel_dis(keep[i]);
  // Таблица инициализации — из драйвера JD9853 Waveshare (esp_lcd_jd9853)
  static const struct {
    uint8_t c, d[32], n;
    uint8_t ms;
  } init[] = {
      {0x11, {0}, 0, 120},
      {0xDF, {0x98, 0x53}, 2, 0}, {0xDF, {0x98, 0x53}, 2, 0}, {0xB2, {0x23}, 1, 0},
      {0xB7, {0x00, 0x47, 0x00, 0x6F}, 4, 0}, {0xBB, {0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
      {0xC0, {0x44, 0xA4}, 2, 0}, {0xC1, {0x16}, 1, 0},
      {0xC3, {0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}, 8, 0},
      {0xC4, {0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82}, 12, 0},
      {0xC8, {0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
              0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00}, 32, 0},
      {0xD0, {0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0}, {0xD7, {0x00, 0x30}, 2, 0}, {0xE6, {0x14}, 1, 0},
      {0xDE, {0x01}, 1, 0}, {0xB7, {0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0}, {0xC1, {0x14, 0x15, 0xC0}, 3, 0},
      {0xC2, {0x06, 0x3A}, 2, 0}, {0xC4, {0x72, 0x12}, 2, 0}, {0xBE, {0x00}, 1, 0}, {0xDE, {0x02}, 1, 0},
      {0xE5, {0x00, 0x02, 0x00}, 3, 0}, {0xE5, {0x01, 0x02, 0x00}, 3, 0}, {0xDE, {0x00}, 1, 0},
      {0x35, {0x00}, 1, 0}, {0x3A, {0x05}, 1, 0}, {0x36, {0x00}, 1, 0}, {0x21, {0}, 0, 0},
      {0xDE, {0x02}, 1, 0}, {0xE5, {0x00, 0x02, 0x00}, 3, 0}, {0xDE, {0x00}, 1, 0}, {0x29, {0}, 0, 20},
  };
  for (size_t i = 0; i < sizeof init / sizeof *init; i++) {
    cmd(init[i].c, init[i].n ? init[i].d : NULL, init[i].n);
    if (init[i].ms) vTaskDelay(pdMS_TO_TICKS(init[i].ms));
  }
  d_clear(0);
  flush_rows(0, LCD_H);
  remember();
  stay_awake(true);
  backlight(duty, 300, false);
}

// Новый кадр. Если интерфейс попросил переход (ui_anim) — сначала промежуточные кадры, и
// только в тех строках, что меняются (строка статуса на главном экране стоит на месте).
// Полный кадр по SPI 80 МГц — около 12 мс, переход — 100–150 мс.
#define ANIM_STEPS_SLIDE 8
#define ANIM_STEPS_FADE 6

void oled_show(void) {
  if (!io) return;
  hold_drawn = false;  // кадр новый: полосу удержания восстанавливать не из чего
  int kind = lcd_anim, y0, y1;
  lcd_anim = ANIM_NONE;
  if (asleep) {
    wake();
    return;
  }
  if (kind != ANIM_NONE && shown && tmp && d_diff_rows(shown, lcd_fb, &y0, &y1)) {
    if (kind != ANIM_FADE) {  // сдвигаем экран целиком: и одинаковые сверху части кружков
      y0 = 0;
      y1 = LCD_H;
    }
    int steps = kind == ANIM_FADE ? ANIM_STEPS_FADE : ANIM_STEPS_SLIDE;
    for (int i = 1; i < steps; i++) {
      d_anim_frame(shown, lcd_fb, kind, i * 256 / steps, y0, y1, tmp);
      flush_from(tmp, y0, y1 - y0);
    }
    flush_rows(y0, y1 - y0);
  } else if (shown && !full_next) {
    // Только изменившиеся строки; тот же кадр (строка статуса перерисована как была) —
    // экран не трогаем вовсе
    if (!d_diff_rows(shown, lcd_fb, &y0, &y1)) return;
    flush_rows(y0, y1 - y0);
  } else {
    flush_rows(0, LCD_H);
  }
  full_next = false;
  remember();
}

// Диагностика панели: весь экран белый / обратно кадр
void oled_all_on(bool on) {
  if (!io) return;
  if (!on) {
    flush_rows(0, LCD_H);
    remember();
    return;
  }
  static uint16_t *keep;  // кадр не портим: белым заливается копия
  if (!keep) keep = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
  if (!keep) return;
  memcpy(keep, lcd_fb, LCD_W * LCD_H * 2);
  d_clear(0xFFFF);
  flush_rows(0, LCD_H);
  memcpy(lcd_fb, keep, LCD_W * LCD_H * 2);
  full_next = true;
}

void oled_com_pins(uint8_t v) { (void)v; }  // у SSD1306 — раскладка COM; здесь нечего менять

// 0..255 -> подсветка; самый тёмный уровень меню (8) остаётся читаемым
void oled_contrast(uint8_t level) {
  duty = 60 + (uint32_t)level * (1023 - 60) / 255;
  if (io && !asleep) backlight(duty, 150, false);
}

void oled_sleep(void) {
  if (!io || asleep) return;
  backlight(0, 250, true);  // плавно погаснуть, потом усыпить контроллер
  cmd(0x28, NULL, 0);
  cmd(0x10, NULL, 0);  // сон контроллера
  asleep = true;
  stay_awake(false);
}

bool oled_is_asleep(void) { return asleep; }

// Полоса вдоль нижнего края, пока держишь кнопку или палец (не портит то, что под ней)
void oled_hold_progress(int percent) {
  if (!io) return;
  uint16_t *last = lcd_fb + (LCD_H - HOLD_H) * LCD_W;
  if (asleep) oled_show();
  if (percent < 0) {
    if (!hold_drawn) return;
    memcpy(last, saved, sizeof saved);
    hold_drawn = false;
    flush_rows(LCD_H - HOLD_H, HOLD_H);
    if (shown) memcpy(shown + (LCD_H - HOLD_H) * LCD_W, last, sizeof saved);
    return;
  }
  if (!hold_drawn) memcpy(saved, last, sizeof saved);
  hold_drawn = true;
  int n = LCD_W * (percent > 100 ? 100 : percent) / 100;
  for (int y = 0; y < HOLD_H; y++)
    for (int x = 0; x < LCD_W; x++) last[y * LCD_W + x] = x < n ? RGB(64, 140, 255) : saved[y * LCD_W + x];
  flush_rows(LCD_H - HOLD_H, HOLD_H);
  if (shown) memcpy(shown + (LCD_H - HOLD_H) * LCD_W, last, sizeof saved);  // shown = что на экране
}
