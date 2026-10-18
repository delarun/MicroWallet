#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "coins.h"  // FW_VERSION, NET_NAME
#include "gfx.h"
#include "hal.h"
#include "icons.h"
#include "oled.h"
#include "storage.h"
#include "ui.h"
#include "ux.h"
#include "wallet.h"

#define ADDR_MAX 128

// Сколько адресов для получения: столько показывает «Receive» (#0..N-1) и столько
// проверяет приложение на телефоне, считая баланс. Больше 20 нельзя: другие
// кошельки при восстановлении сида дальше 20 пустых адресов подряд не ищут.
static const uint8_t ADDRS_N[4] = {1, 5, 10, 20};

static const uint8_t BRIGHTNESS[4] = {0x08, 0x30, 0x8F, 0xFF};
static const uint32_t SCREEN_OFF_S[4] = {30, 60, 300, 0};
static const char *const SCREEN_OFF_TXT[4] = {"after 30 s", "after 1 min", "after 5 min",
                                              "never"};

// Выключение (глубокий сон): после скольких минут простоя. По умолчанию —
// 2 минуты: через минуту экран гаснет (лёгкий сон), ещё через минуту — глубокий.
static const uint32_t POWER_OFF_S[4] = {120, 300, 900, 0};
static const char *const POWER_OFF_TXT[4] = {"after 2 min", "after 5 min", "after 15 min", "never"};

// Батарея читается не чаще раза в 2 секунды (АЦП + усреднение)
static int battery_mv(void) {
  static int mv = -1;
  static uint64_t at;
  uint64_t now = hal_time_us();
  if (!at || now - at > 2000000) {
    mv = hal_battery_mv();
    at = now;
  }
  return mv;
}

int menu_battery_mv(void) { return battery_mv(); }
int menu_battery_pct(void) {
  int mv = battery_mv();
  return hal_charging() ? UI_BATT_CHARGING : ui_battery_pct(mv);
}

// Монеты — приложения WebAssembly (src/apps.c): главный экран — их иконки и настройки
typedef enum { S_DASH, S_ADDR, S_SETTINGS, S_APP } state_t;
enum { AI_RECEIVE, AI_ABOUT, AI_REMOVE, AI_BACK, AI_COUNT };  // меню приложения
enum { SI_BRIGHT, SI_SCREEN, SI_POWER, SI_ADDRS, SI_OFFNOW, SI_BATT, SI_BLE, SI_FORGET, SI_ABOUT, SI_WIPE, SI_BACK, SI_MAX };

static state_t st;
static int dash_i, coin, coin_i, set_i, addr_page;
static uint32_t addr_idx;
static settings_t cfg;

// Адрес считает приложение (для Pearl — три умножения точки), кэшируем
static char addr_cache[ADDR_MAX];
static int cache_coin = -1;
static uint32_t cache_idx;

static const mwapp_t *coin_app(int c) { return apps_get(c); }

static const char *address(int c, uint32_t i) {
  if (c == cache_coin && i == cache_idx) return addr_cache;
  char cmd[16];
  snprintf(cmd, sizeof cmd, "addr %lu", (unsigned long)i);
  if (!coin_app(c) || !apps_query(coin_app(c), cmd, addr_cache, sizeof addr_cache)) {
    cache_coin = -1;
    return "app error";
  }
  cache_coin = c;
  cache_idx = i;
  return addr_cache;
}

// Пункты настроек зависят от платы (BLE) и наличия кошелька (сброс)
static int settings_items(int out[SI_MAX]) {
  int n = 0;
  for (int i = 0; i < SI_MAX; i++) {
    if ((i == SI_BLE || i == SI_FORGET) && !hal_ble_available()) continue;
    if (i == SI_BATT && battery_mv() < 0 && !hal_charging()) continue;
    if ((i == SI_POWER || i == SI_OFFNOW) && !hal_power_off_available()) continue;
    if (i == SI_WIPE && !wallet_is_unlocked()) continue;
    out[n++] = i;
  }
  return n;
}

// Без приложений на главном экране — подсказка «поставь с телефона» и настройки
static int dash_count(void) { return wallet_is_unlocked() ? (apps_count() ? apps_count() : 1) + 1 : 2; }

void menu_init(void) {
  settings_load(&cfg);
  if (cfg.brightness > 3) cfg.brightness = 2;
  if (cfg.screen_off > 3) cfg.screen_off = 1;
  if (cfg.power_off > 3) cfg.power_off = 0;
  if (cfg.addrs > 3) cfg.addrs = 3;
  oled_contrast(BRIGHTNESS[cfg.brightness]);
  if (hal_ble_available()) hal_ble_set_enabled(!cfg.ble_off);
  menu_reset();
}

void menu_reset(void) {
  st = S_DASH;
  dash_i = 0;
  cache_coin = -1;
}

bool menu_on_dashboard(void) { return st == S_DASH; }

uint32_t menu_power_off_ms(void) {
  return hal_power_off_available() ? POWER_OFF_S[cfg.power_off] * 1000u : 0;
}

uint32_t menu_screen_off_ms(void) { return SCREEN_OFF_S[cfg.screen_off] * 1000u; }

int menu_addr_count(void) { return ADDRS_N[cfg.addrs]; }

bool menu_set_addr_count(int n) {
  for (int i = 0; i < 4; i++)
    if (ADDRS_N[i] == n) {
      cfg.addrs = (uint8_t)i;
      return settings_save(&cfg);
    }
  return false;
}

static void draw_settings(void) {
  char sub[24];
  int items[SI_MAX], n = settings_items(items);
  if (set_i >= n) set_i = 0;
  switch (items[set_i]) {
    case SI_BRIGHT:
      snprintf(sub, sizeof sub, "level %d of 4", cfg.brightness + 1);
      ui_carousel(ICON_SUN, "Brightness", sub, set_i, n);
      break;
    case SI_SCREEN:  // экран гаснет, плата уходит в лёгкий сон
      ui_carousel(ICON_MOON, "Sleep", SCREEN_OFF_TXT[cfg.screen_off], set_i, n);
      break;
    case SI_POWER:
      ui_carousel(ICON_LOCK, "Power off", POWER_OFF_TXT[cfg.power_off], set_i, n);
      break;
    case SI_ADDRS:
      if (ADDRS_N[cfg.addrs] == 1) snprintf(sub, sizeof sub, "only #0");
      else snprintf(sub, sizeof sub, "#0 to #%d", ADDRS_N[cfg.addrs] - 1);
      ui_carousel(ICON_RECEIVE, "Addresses", sub, set_i, n);
      break;
    case SI_OFFNOW:
      ui_carousel(ICON_MOON, "Turn off", "button wakes it", set_i, n);
      break;
    case SI_BATT: {
      int mv = battery_mv();
      if (hal_charging()) snprintf(sub, sizeof sub, "charging (USB)");
      else snprintf(sub, sizeof sub, "%d.%02d V  %d%%", mv / 1000, mv % 1000 / 10, ui_battery_pct(mv));
      ui_carousel(ICON_BATT, "Battery", sub, set_i, n);
      break;
    }
    case SI_BLE:
      ui_carousel(ICON_BT, "Bluetooth",
                  !hal_ble_enabled() ? "off" : hal_ble_connected() ? "on, connected" : "on",
                  set_i, n);
      break;
    case SI_FORGET:
      ui_carousel(ICON_TRASH, "Forget PCs", "remove BLE pairings", set_i, n);
      break;
    case SI_ABOUT:
      ui_carousel(ICON_INFO, "MicroWallet", "v" FW_VERSION "  " NET_NAME, set_i, n);
      break;
    case SI_WIPE:
      ui_carousel(ICON_TRASH, "Wipe device", "erase the seed", set_i, n);
      break;
    default:
      ui_carousel(ICON_BACK, "Back", NULL, set_i, n);
  }
}

void menu_draw(void) {
  oled_all_on(false);  // после диагностики экрана
  char t[32];
  switch (st) {
    case S_DASH:
      if (!wallet_is_unlocked()) {
        static const uint8_t *const icons[] = {ICON_PLUS, ICON_GEAR};
        if (dash_i == 0) ui_dashboard(icons, 2, 0, "New wallet", "hold to create");
        else ui_dashboard(icons, 2, 1, "Settings", NULL);
      } else {
        // Строка иконок — окно до 9 штук вокруг выбранной, если приложений много
        enum { VISIBLE = 9 };
        const uint8_t *icons[VISIBLE];
        int n = dash_count(), first = 0, shown = n < VISIBLE ? n : VISIBLE;
        if (n > VISIBLE) {
          first = dash_i - VISIBLE / 2;
          if (first < 0) first = 0;
          if (first > n - VISIBLE) first = n - VISIBLE;
        }
        for (int k = 0; k < shown; k++) {
          int i = first + k;
          icons[k] = i == n - 1 ? ICON_GEAR : apps_count() ? coin_app(i)->icon : ICON_PLUS;
        }
        int sel = dash_i - first;
        if (dash_i == n - 1) {
          ui_dashboard(icons, shown, sel, "Settings", "brightness, wipe");
        } else if (!apps_count()) {
          ui_dashboard(icons, shown, sel, "Add coins", "install apps from phone");
        } else {
          snprintf(t, sizeof t, "v%s  %s", coin_app(dash_i)->version, NET_NAME);
          ui_dashboard(icons, shown, sel, coin_app(dash_i)->name, t);
        }
      }
      ui_status(NET_NAME, hal_ble_enabled(), hal_ble_connected(), hal_usb_connected(),
                menu_battery_pct());
      break;
    case S_ADDR: {
      const mwapp_t *app = coin_app(coin);
      if (!app) break;
      const char *a = address(coin, addr_idx);
      snprintf(t, sizeof t, "%s #%lu", app->name, (unsigned long)addr_idx);
      ui_field(app->icon, t, a, addr_page, ui_pages(a), -1, 0);
      break;
    }
    case S_APP: {
      const mwapp_t *a = coin_app(coin);
      char paths[40], about[64];
      if (!a) break;
      apps_paths_str(a, paths, sizeof paths);
      if (coin_i == AI_RECEIVE) ui_carousel(ICON_RECEIVE, "Receive", "show address", coin_i, AI_COUNT);
      else if (coin_i == AI_ABOUT) {
        snprintf(about, sizeof about, "v%s  %s", a->version, paths);
        ui_carousel(a->icon, a->name, about, coin_i, AI_COUNT);
      } else if (coin_i == AI_REMOVE) ui_carousel(ICON_TRASH, "Remove app", "seed is not touched", coin_i, AI_COUNT);
      else ui_carousel(ICON_BACK, "Back", NULL, coin_i, AI_COUNT);
      break;
    }
    case S_SETTINGS:
      draw_settings();
      break;
  }
  oled_show();
}

static void settings_hold(void) {
  int items[SI_MAX];
  settings_items(items);
  switch (items[set_i]) {
    case SI_BRIGHT:
      cfg.brightness = (cfg.brightness + 1) % 4;
      oled_contrast(BRIGHTNESS[cfg.brightness]);
      settings_save(&cfg);
      break;
    case SI_SCREEN:
      cfg.screen_off = (cfg.screen_off + 1) % 4;
      settings_save(&cfg);
      break;
    case SI_POWER:
      cfg.power_off = (cfg.power_off + 1) % 4;
      settings_save(&cfg);
      break;
    case SI_ADDRS:
      cfg.addrs = (cfg.addrs + 1) % 4;
      settings_save(&cfg);
      break;
    case SI_OFFNOW:
      app_power_off();
      break;
    case SI_BLE:
      cfg.ble_off = hal_ble_enabled();
      hal_ble_set_enabled(!cfg.ble_off);
      settings_save(&cfg);
      break;
    case SI_FORGET:
      hal_ble_forget_all();
      ux_flash(ICON_CHECK, "Forgotten", "pair again from PC", 1200);
      break;
    case SI_WIPE:
      app_wipe();
      if (!wallet_is_unlocked()) menu_reset();
      break;
    case SI_BACK:
      st = S_DASH;
      break;
  }
}

void menu_event(btn_t b) {
  if (b == BTN_NONE) return;
  bool tap = b == BTN_TAP;
  switch (st) {
    case S_DASH:
      if (tap) {
        dash_i = (dash_i + 1) % dash_count();
      } else if (!wallet_is_unlocked() && dash_i == 0) {
        app_create_wallet();
        menu_reset();
      } else if (dash_i == dash_count() - 1) {
        st = S_SETTINGS;
        set_i = 0;
      } else if (apps_count()) {
        st = S_APP;
        coin = dash_i;
        coin_i = 0;
        cache_coin = -1;
      }
      break;
    case S_APP:
      if (tap) {
        coin_i = (coin_i + 1) % AI_COUNT;
      } else if (coin_i == AI_RECEIVE) {
        st = S_ADDR;
        addr_idx = 0;
        addr_page = 0;
      } else if (coin_i == AI_REMOVE) {
        if (coin_app(coin)) apps_remove(coin_app(coin));  // сам вернёт на главный экран
        return;
      } else if (coin_i == AI_BACK) {
        st = S_DASH;
      }
      break;
    case S_ADDR:
      if (!tap) {
        st = S_APP;  // удержание — назад
      } else if (++addr_page >= ui_pages(address(coin, addr_idx))) {
        addr_page = 0;  // последняя страница — следующий адрес
        addr_idx = (addr_idx + 1) % (uint32_t)menu_addr_count();
      }
      break;
    case S_SETTINGS: {
      int items[SI_MAX], n = settings_items(items);
      if (tap) set_i = (set_i + 1) % n;
      else settings_hold();
      break;
    }
  }
  menu_draw();
}
