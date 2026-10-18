// app.c — ядро MicroWallet: общее для ESP32-S3 и симулятора на ПК.
//
// Интерфейс — главный экран (menu.c) и пошаговый просмотр (ux.c): короткое
// нажатие — дальше, удержание — выбрать. Плата даёт только hal.h и цикл,
// который передаёт сюда строки команд (app_line) и тики (app_tick).
//
// Протокол: текстовые строки, ответ "ok ..." или "err ...". Одинаков для USB
// и для шифрованного BLE-канала (channel.c).
//   ping | info | wipe | restore <entropy hex>
//   boot  — перезагрузка в режим прошивки
//   BTC:  xpub | addr <chain> <i> | show <chain> <i> | sign <hex>
//   TRON: tron addr <i> | tron show <i> | tron sign <i> <raw_data hex>
//   BSC:  bsc addr <i>  | bsc show <i>  | bsc sign <i> <unsigned rlp hex>
//   PRL:  prl addr <chain> <i> | prl show <chain> <i> | prl sign <hex>
//   show, sign, wipe и restore ждут подтверждения на устройстве, иначе "err rejected".
//   tron/bsc sign отвечают "ok <r><s><recid>" (65 байт hex),
//   prl sign — "ok <sig0> <sig1> ..." (64-байтные BIP340-подписи по входам)
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bip39.h"
#include "ecdsa.h"
#include "memzero.h"
#include "secp256k1.h"
#include "sha2.h"
#include "sha3.h"

#include "apps.h"
#include "button.h"
#include "coins.h"
#include "gfx.h"
#include "hal.h"
#include "icons.h"
#include "menu.h"
#include "oled.h"
#include "storage.h"
#include "ui.h"
#include "ux.h"
#include "wallet.h"

// ---------- хуки для trezor-crypto ----------

void random_buffer(uint8_t *buf, size_t len) { hal_random(buf, len); }

static uint32_t rand32(void) {
  uint32_t r;
  hal_random((uint8_t *)&r, sizeof r);
  return r;
}

void tc_fault_handler(const char *msg) {
  (void)msg;
  ui_message(ICON_WARN, "Crypto fault", "halted");
  oled_show();
  for (;;) hal_sleep_ms(1000);
}

// ---------- ответ хосту ----------
// Ответ уходит туда, откуда пришла команда: в USB или в BLE-канал

static app_reply_fn g_reply;

static void reply(const char *fmt, ...) {
  static char buf[1024];  // ответы самой прошивки короткие; подписи монет отвечают приложения
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (g_reply) g_reply(buf);
  memzero(buf, sizeof buf);
}

// ---------- утилиты ----------

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static long from_hex(const char *s, uint8_t *out, size_t max) {
  size_t n = strlen(s);
  if (n % 2 || n / 2 > max) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
    if (h < 0 || l < 0) return -1;
    out[i] = (uint8_t)(h << 4 | l);
  }
  return (long)(n / 2);
}

// ---------- энтропия ----------
// Плата даёт 32 байта (аппаратный TRNG ESP32-S3), сюда же подмешиваем время и
// длительность нажатия кнопки.
static void gen_entropy(uint8_t out[16]) {
  SHA256_CTX c;
  uint8_t h[32];
  sha256_Init(&c);
  for (int i = 0; i < 8; i++) {
    hal_entropy(h);
    sha256_Update(&c, h, sizeof h);
    uint64_t t = hal_time_us();
    sha256_Update(&c, (const uint8_t *)&t, 8);
  }
  uint32_t press = btn_last_press_us();
  sha256_Update(&c, (const uint8_t *)&press, 4);
  sha256_Final(&c, h);
  memcpy(out, h, 16);
  memzero(h, sizeof h);
  memzero(&c, sizeof c);
}

// ---------- создание кошелька ----------

// Три варианта по кругу, удержание — выбрать
static bool quiz_word(int idx, const char *correct) {
  const char *opt[3];
  int pos = rand32() % 3;
  for (int k = 0; k < 3; k++) {
    opt[k] = correct;
    if (k != pos) {
      do { opt[k] = mnemonic_get_word(rand32() % 2048); } while (!strcmp(opt[k], correct));
    }
  }
  char title[24];
  snprintf(title, sizeof title, "Word #%d is...?", idx + 1);
  for (int k = 0;; k = (k + 1) % 3) {
    ui_word(title, opt[k], k, 3);
    oled_show();
    if (btn_wait() == BTN_HOLD) return k == pos;
  }
}

static void try_unlock(void);

// Пороги по аккумулятору (без USB). Ниже LOW тяжёлые операции не начинаем: запись во
// флеш и PBKDF2 дают пик тока, и напряжение платы может провалиться. Ниже CRITICAL —
// выключаемся сами, пока не случилось зависание (см. просадку в sdkconfig.defaults).
#define BATT_LOW_MV 3550
#define BATT_CRITICAL_MV 3450

static bool battery_below(int lim) {
  int mv = menu_battery_mv();
  return !hal_charging() && mv > 0 && mv < lim;
}

bool app_battery_low(void) {
  if (!battery_below(BATT_LOW_MV)) return false;
  ux_flash(ICON_WARN, "Battery low", "connect USB first", 2500);
  return true;
}

bool app_create_wallet(void) {
  static char words[12][12];
  uint8_t ent[16];

  if (app_battery_low()) return false;

  ui_message(ICON_KEY, "Generating", "new seed");
  oled_show();
  gen_entropy(ent);
  const char *m = mnemonic_from_data(ent, sizeof ent);
  if (!m) return false;
  int wi = 0, ci = 0;
  for (const char *p = m; *p && wi < 12; p++) {
    if (*p == ' ') { words[wi][ci] = 0; wi++; ci = 0; }
    else if (ci < 11) words[wi][ci++] = *p;
  }
  if (wi < 12) words[wi][ci] = 0;
  mnemonic_clear();

  for (;;) {
    ux_flash(ICON_PEN, "Write down", "12 words on paper", 0);
    for (int i = 0; i < 12; i++) {
      char title[24];
      snprintf(title, sizeof title, "Word %d of 12", i + 1);
      ui_word(title, words[i], i, 12);
      oled_show();
      btn_wait();
    }
    ux_flash(ICON_EYE, "Check backup", "hold on right word", 0);
    int a = rand32() % 12, b;
    do { b = rand32() % 12; } while (b == a);
    if (quiz_word(a, words[a]) && quiz_word(b, words[b])) break;
    ux_flash(ICON_CROSS, "Wrong word", "let's start over", 0);
  }

  ui_message(ICON_LOCK, "Saving", NULL);
  oled_show();
  bool ok = storage_save(ent, sizeof ent);
  memzero(ent, sizeof ent);
  memzero(words, sizeof words);
  if (!ok) {
    ux_error("flash write failed");
    return false;
  }
  try_unlock();
  ux_flash(ICON_CHECK, "Wallet ready", NULL, 1200);
  return true;
}

// ---------- разблокировка, сброс ----------

static void unlock_progress(uint32_t cur, uint32_t total) {
  static int last = -1;
  int p = total ? (int)(cur * 100 / total) : 0;
  if (p / 5 == last / 5) return;
  last = p;
  ui_progress(ICON_LOCK, "Unlocking", p);
  oled_show();
}

static void try_unlock(void) {
  uint8_t ent[32];
  size_t n;
  if (!storage_load(ent, &n)) return;
  unlock_progress(0, 1);
  if (!wallet_unlock(ent, n, unlock_progress)) ux_error("unlock failed");
  memzero(ent, sizeof ent);
}

static bool wipe_flow(void) {
  static const ux_step_t st[] = {
      {UX_TITLE, ICON_WARN, "Wipe device?", "seed will be erased"},
      {UX_REJECT, ICON_CROSS, "Cancel", "hold to keep"},
      {UX_ACCEPT, ICON_TRASH, "Wipe", "hold to erase"},
  };
  if (!ux_review(st, 3)) return false;
  storage_wipe();
  wallet_lock();
  ux_flash(ICON_CHECK, "Wiped", NULL, 1200);
  return true;
}

void app_wipe(void) { wipe_flow(); }

// После экрана подтверждения: ответ хосту и короткое сообщение
static void finish(const char *err) {
  if (err) {
    reply("err %s", err);
    if (!strcmp(err, "rejected")) ux_flash(ICON_CROSS, "Rejected", NULL, 900);
    else ux_error(err);
  }
  menu_draw();
}

// ---------- восстановление из seed (только на пустом устройстве) ----------

static void restore_flow(const char *hex) {
  static uint8_t ent[32];
  static char words[24][12], summary[96];
  long n = hex ? from_hex(hex, ent, sizeof ent) : -1;
  if (n != 16 && n != 24 && n != 32) { reply("err bad entropy"); return; }
  if (app_battery_low()) {
    reply("err battery low, connect USB");
    menu_draw();
    return;
  }
  const char *m = mnemonic_from_data(ent, (int)n);
  if (!m) { reply("err bad entropy"); return; }
  int wi = 0, ci = 0;
  for (const char *p = m; *p && wi < 24; p++) {
    if (*p == ' ') { words[wi][ci] = 0; wi++; ci = 0; }
    else if (ci < 11) words[wi][ci++] = *p;
  }
  if (wi < 24) words[wi++][ci] = 0;
  mnemonic_clear();
  // Первое и последнее слово — чтобы сверить с бумагой
  snprintf(summary, sizeof summary, "%d words: %.11s ... %.11s", wi, words[0], words[wi - 1]);
  const ux_step_t st[] = {
      {UX_TITLE, ICON_KEY, "Restore", "seed from PC"},
      {UX_FIELD, ICON_PEN, "Seed", summary},
      {UX_TITLE, ICON_WARN, "PC saw seed", "trust this PC?"},
      {UX_ACCEPT, ICON_CHECK, "Restore", "hold to save"},
      {UX_REJECT, ICON_CROSS, "Reject", "hold to cancel"},
  };
  bool ok = ux_review(st, 5);
  memzero(words, sizeof words);
  memzero(summary, sizeof summary);
  if (!ok) {
    memzero(ent, sizeof ent);
    finish("rejected");
    return;
  }
  ui_message(ICON_LOCK, "Saving", NULL);
  oled_show();
  ok = storage_save(ent, (size_t)n);
  memzero(ent, sizeof ent);
  if (!ok) {
    finish("flash write failed");
    return;
  }
  try_unlock();
  reply("ok restored");
  ux_flash(ICON_CHECK, "Wallet ready", NULL, 1200);
  menu_reset();
  menu_draw();
}

bool app_pair_confirm(const char *code) {
  const ux_step_t st[] = {
      {UX_TITLE, ICON_BT, "Pair with PC?", "Bluetooth"},
      {UX_AMOUNT, ICON_KEY, "Code", code},
      {UX_ACCEPT, ICON_CHECK, "Pair", "if codes match"},
      {UX_REJECT, ICON_CROSS, "Reject", "hold to refuse"},
  };
  bool ok = ux_review(st, 4);
  ux_flash(ok ? ICON_CHECK : ICON_CROSS, ok ? "Paired" : "Rejected", NULL, 800);
  menu_draw();
  return ok;
}

static void handle(char *line) {
  if (apps_line(line, g_reply)) return;  // apps, app ... (WebAssembly)
  char *cmd = strtok(line, " ");
  if (!cmd) return;
  char *a1 = strtok(NULL, " ");

  if (!strcmp(cmd, "ping")) { reply("ok pong"); return; }
  if (!strcmp(cmd, "boot")) {
    reply("ok rebooting to USB bootloader");
    ui_message(ICON_SEND, "Update mode", "flash with esptool");
    oled_show();
    hal_sleep_ms(100);
    wallet_lock();
    hal_reboot_bootloader();
  }
  if (!strcmp(cmd, "info")) {
    uint32_t hf, hl;
    hal_heap(&hf, &hl);
    char coins[64] = "";  // монеты — это установленные приложения
    for (int i = 0, o = 0; i < apps_count() && o < (int)sizeof coins - 10; i++)
      o += snprintf(coins + o, sizeof coins - o, "%s%s", i ? "," : "", apps_get(i)->id);
    reply("ok fw=" FW_VERSION " net=" NET_NAME " state=%s coins=%s hw=%s batt=%d chg=%d build=%s boot=%s heap=%lu/%lu apps=%d apps_free=%d addrs=%d secure=%d ota=%s",
           wallet_is_unlocked() ? "ready" : "empty", coins, hal_platform(), menu_battery_mv(), hal_charging(), hal_fw_build(),
           hal_boot_reason(), (unsigned long)hf, (unsigned long)hl, (int)(apps_total() / 1024),
           (int)(apps_free() / 1024), menu_addr_count(), hal_secure(), hal_ota_info());
    return;
  }
  if (!strcmp(cmd, "fingerprint")) {  // отпечаток мастер-ключа (не секрет): для PSBT и Sparrow
    uint32_t fp;
    if (!wallet_master_fingerprint(&fp)) reply("err no wallet on device");
    else reply("ok %08lx", (unsigned long)fp);
    return;
  }
  if (!strcmp(cmd, "addrs")) {  // addrs <1|5|10|20>: сколько адресов для получения
    if (!a1 || !menu_set_addr_count(atoi(a1))) {
      reply("err usage: addrs 1|5|10|20");
      return;
    }
    reply("ok addrs=%d", menu_addr_count());
    return;
  }
  if (!strcmp(cmd, "oled")) {  // диагностика экрана: oled [all | раскладка COM, hex]
    if (a1 && !strcmp(a1, "all")) {  // все пиксели горят: тёмные строки — дефект панели
      oled_all_on(true);
      reply("ok all pixels on, press button to exit");
      return;
    }
    oled_all_on(false);
    if (a1) oled_com_pins((uint8_t)strtoul(a1, NULL, 16));
    gfx_clear();
    gfx_fill(0, 0, GFX_W, 1, true);
    gfx_fill(0, GFX_H - 1, GFX_W, 1, true);
    gfx_fill(0, 0, 1, GFX_H, true);
    gfx_fill(GFX_W - 1, 0, 1, GFX_H, true);
    // Сплошной прямоугольник (на глаз должен быть без полос), шахматка 2x2
    // и крупный текст через границу страниц (строки 7/8)
    gfx_fill(4, 4, 40, GFX_H - 8, true);
    for (int y = 4; y < GFX_H - 4; y++)
      for (int x = 50; x < 74; x++) gfx_pixel(x, y, ((x >> 1) + (y >> 1)) & 1);
    gfx_text(80, 2, a1 ? a1 : "AB", GFX_BIG);
    gfx_text(80, GFX_H / 2 + 4, "88", GFX_BIG);
    oled_show();
    reply("ok height=%d", GFX_H);
    return;
  }
  if (!strcmp(cmd, "restore")) {
    if (wallet_is_unlocked()) reply("err wallet exists");
    else restore_flow(a1);
    return;
  }
  if (!wallet_is_unlocked()) { reply("err no wallet"); return; }

  // Монеты — приложения: app btc ..., app tron ... (apps_line выше)
  if (!strcmp(cmd, "wipe")) {
    if (wipe_flow()) {
      reply("ok wiped");
      menu_reset();
      menu_draw();
    } else {
      finish("rejected");
    }
  } else {
    reply("err unknown command");
  }
}

// ---------- вход для платформы ----------

static uint64_t last_input;
static bool swallow;

void app_power_off(void) {
  ui_message(ICON_MOON, "Power off", "press button to wake");
  oled_show();
  hal_sleep_ms(1200);
  // Пробуждение — по уровню: нажатая кнопка или дребезг при отпускании разбудили бы
  // сразу. Ждём, пока кнопка 0.3 с подряд отпущена.
  for (int quiet = 0; quiet < 30;) {
    hal_sleep_ms(10);
    quiet = btn_pressed() ? 0 : quiet + 1;
  }
  oled_sleep();
  hal_power_off();
}

void app_init(void) {
  btn_init();
  oled_init();
  apps_init();
  menu_init();
  if (!hal_resumed()) {  // после пробуждения — сразу к делу, без заставки
    ui_splash(FW_VERSION, NET_NAME);
    oled_show();
    hal_sleep_ms(1500);
    // Защищён ли seed — по eFuse платы (Secure Boot + шифрование флеша и NVS)
    if (hal_secure()) ui_message(ICON_LOCK, "Encrypted", "seed protected");
    else ui_message(ICON_WARN, "Not encrypted", "seed is readable");
    oled_show();
    hal_sleep_ms(hal_secure() ? 1200 : 2500);
  }
  try_unlock();
  menu_draw();
  last_input = hal_time_us();
}

static app_ext_fn g_ext;
static bool g_busy;

void app_set_ext(app_ext_fn fn) { g_ext = fn; }

bool app_is_busy(void) { return g_busy; }

void app_busy(bool on) {
  g_busy = on;
  last_input = hal_time_us();
}

void app_line(char *line, app_reply_fn out) {
  if (!g_ext || !g_ext(line, out)) {
    g_reply = out;
    handle(line);
    g_reply = NULL;
  }
  last_input = hal_time_us();  // пока идут команды (и обновление), экран не гаснет
}

void app_tick(void) {
  // Экран погашен: первое нажатие только будит его
  btn_t b = btn_poll();
  uint64_t now = hal_time_us();
  if (g_busy) {
    last_input = now;
    return;
  }
  if (oled_is_asleep() && btn_pressed()) {
    menu_draw();
    swallow = true;
    last_input = now;
  }
  if (b != BTN_NONE) {
    last_input = now;
    if (swallow) swallow = false;
    else menu_event(b);
  }
  uint32_t off = menu_screen_off_ms();
  if (off && !oled_is_asleep() && now - last_input > (uint64_t)off * 1000) oled_sleep();

  // Аккумулятор на исходе — выключиться, пока плата не зависла от просадки
  static uint64_t batt_checked;
  if (now - batt_checked > 10000000) {  // раз в 10 с: АЦП и так усредняет
    batt_checked = now;
    if (hal_power_off_available() && battery_below(BATT_CRITICAL_MV)) {
      ui_message(ICON_WARN, "Battery empty", "charge over USB");
      oled_show();
      hal_sleep_ms(2500);
      app_power_off();
    }
  }

  // Долгий простой — выключиться (глубокий сон), если никто не на связи
  uint32_t poff = menu_power_off_ms();
  if (poff && now - last_input > (uint64_t)poff * 1000 && !hal_usb_connected() &&
      !hal_ble_connected() && !btn_pressed())
    app_power_off();

  // Экран погашен — плата может спать (лёгкий сон, BLE остаётся на связи)
  static bool saving;
  if (oled_is_asleep() != saving) {
    saving = oled_is_asleep();
    hal_power_save(saving);
  }

  // Сменилось состояние связи — обновить строку статуса на главном экране
  static int last_link = -1;
  int link = hal_ble_enabled() | hal_ble_connected() << 1 | hal_usb_connected() << 2 |
             (menu_battery_pct() + 2) << 3;  // -2 — зарядка
  if (link != last_link) {
    last_link = link;
    if (menu_on_dashboard() && !oled_is_asleep()) menu_draw();
  }
}

void app_redraw(void) { menu_draw(); }
