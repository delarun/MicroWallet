#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps.h"
#include "coins.h"  // FW_VERSION, NET_NAME
#include "gfx.h"
#include "hal.h"
#include "icons.h"
#include "oled.h"
#include "pin.h"
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

// Заряд Li-ion (для ui.h: общий у обоих интерфейсов) по напряжению без нагрузки (кривая разряда, линейно между точками).
// 0% — 3.5 В: ниже плата не работает (диод ~0.35 В и стабилизатор 3.3 В между ними).
int ui_battery_pct(int mv) {
  static const int16_t v[] = {3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  static const uint8_t p[] = {0, 8, 20, 30, 40, 50, 60, 75, 90, 100};
  if (mv < 0) return -1;
  if (mv <= v[0]) return 0;
  for (int i = 1; i < (int)sizeof v / 2; i++)
    if (mv <= v[i]) return p[i - 1] + (p[i] - p[i - 1]) * (mv - v[i - 1]) / (v[i] - v[i - 1]);
  return 100;
}

int menu_battery_mv(void) { return battery_mv(); }
int menu_battery_pct(void) {
  int mv = battery_mv();
  return hal_charging() ? UI_BATT_CHARGING : ui_battery_pct(mv);
}

// Монеты — приложения WebAssembly (src/apps.c): главный экран — их иконки и настройки
typedef enum { S_DASH, S_ADDR, S_SETTINGS, S_APP, S_BAL } state_t;
enum { AI_BALANCE, AI_RECEIVE, AI_ABOUT, AI_REMOVE, AI_BACK, AI_COUNT };  // меню приложения
enum { SI_BRIGHT, SI_SCREEN, SI_POWER, SI_ADDRS, SI_PIN, SI_PINOFF, SI_OFFNOW, SI_BATT, SI_BLE, SI_FORGET, SI_ABOUT,
       SI_WIPE, SI_BACK, SI_MAX };

static state_t st;
static int dash_i, coin, coin_i, set_i, addr_page, bal_page;
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

// Баланс из кэша (команда bal): первая сумма, сколько ещё строк, когда обновлён ("29.10 23:40")
typedef struct {
  char text[BAL_TEXT_MAX + 1], first[40], when[16];
  int more;
} balance_t;

static bool balance_get(const mwapp_t *a, balance_t *b) {
  uint32_t w;
  if (!a || !storage_balance_load(a->id, &w, b->text, sizeof b->text)) return false;
  int n = (int)strcspn(b->text, ";");
  snprintf(b->first, sizeof b->first, "%.*s", n, b->text);
  b->more = 0;
  for (const char *c = b->text; *c; c++) b->more += *c == ';' && c[1];
  time_t t = (time_t)w;  // местное время телефона — показываем как есть
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(b->when, sizeof b->when, "%d.%m %H:%M", &tm);
  return true;
}

void menu_balance_changed(const char *id) {
  const mwapp_t *a = (st == S_APP || st == S_BAL) ? coin_app(coin) : NULL;
  if (a && !strcmp(a->id, id)) menu_draw();
}

// Пункты настроек зависят от платы (BLE) и наличия кошелька (сброс)
static int settings_items(int out[SI_MAX]) {
  int n = 0;
  for (int i = 0; i < SI_MAX; i++) {
    if ((i == SI_BLE || i == SI_FORGET) && !hal_ble_available()) continue;
    if (i == SI_BATT && battery_mv() < 0 && !hal_charging()) continue;
    if ((i == SI_POWER || i == SI_OFFNOW) && !hal_power_off_available()) continue;
    if ((i == SI_WIPE || i == SI_PIN) && !wallet_is_unlocked()) continue;
    if (i == SI_PINOFF && (!wallet_is_unlocked() || !pin_is_set())) continue;
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
  char sub[40];
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
    case SI_PIN:
      ui_carousel(ICON_LOCK, "PIN", pin_is_set() ? "on - hold to change" : "off - hold to set", set_i, n);
      break;
    case SI_PINOFF:
      ui_carousel(ICON_LOCK, "Remove PIN", "hold to remove", set_i, n);
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
  if (app_locked()) {  // главного экрана нет, пока не введён PIN
    app_lock_redraw();
    return;
  }
  char t[32];
  switch (st) {
    case S_DASH:
      if (!wallet_is_unlocked()) {
        static const uint8_t *const icons[] = {ICON_PLUS, ICON_GEAR};
        static const char *const names[] = {"New wallet", "Settings"};
        if (dash_i == 0) ui_dashboard(icons, names, 2, 0, "New wallet", "hold to create");
        else ui_dashboard(icons, names, 2, 1, "Settings", NULL);
      } else {
        static const uint8_t *icons[UI_DASH_MAX];
        static const char *names[UI_DASH_MAX];
        int n = dash_count();
        if (n > UI_DASH_MAX) n = UI_DASH_MAX;
        for (int i = 0; i < n; i++) {
          bool gear = i == n - 1;
          icons[i] = gear ? ICON_GEAR : apps_count() ? coin_app(i)->icon : ICON_PLUS;
          names[i] = gear ? "Settings" : apps_count() ? coin_app(i)->name : "Add coins";
        }
        if (dash_i == n - 1) {
          ui_dashboard(icons, names, n, dash_i, "Settings", "brightness, wipe");
        } else if (!apps_count()) {
          ui_dashboard(icons, names, n, dash_i, "Add coins", "install apps from phone");
        } else {
          snprintf(t, sizeof t, "v%s  %s", coin_app(dash_i)->version, NET_NAME);
          ui_dashboard(icons, names, n, dash_i, coin_app(dash_i)->name, t);
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
      ui_receive(app->icon, t, a, addr_page, ui_receive_pages(a));
      break;
    }
    case S_APP: {
      const mwapp_t *a = coin_app(coin);
      char paths[40], about[64];
      if (!a) break;
      apps_paths_str(a, paths, sizeof paths);
      balance_t b;
      if (coin_i == AI_BALANCE) {
        if (!balance_get(a, &b)) {
          ui_carousel(ICON_COIN, "Balance", "open the app on phone", coin_i, AI_COUNT);
        } else {
          char sub[48];
          if (b.more) snprintf(sub, sizeof sub, "%s  +%d more", b.when, b.more);
          else snprintf(sub, sizeof sub, "%s", b.when);
          ui_carousel(ICON_COIN, b.first, sub, coin_i, AI_COUNT);
        }
      } else if (coin_i == AI_RECEIVE) ui_carousel(ICON_RECEIVE, "Receive", "show address", coin_i, AI_COUNT);
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
    case S_BAL: {  // все суммы монеты и токенов
      balance_t b;
      if (!balance_get(coin_app(coin), &b)) {
        st = S_APP;
        menu_draw();
        return;
      }
      int pages = ui_balance_pages(b.text);
      if (bal_page >= pages) bal_page = pages - 1;
      ui_balance(ICON_COIN, b.when, b.text, bal_page, pages);
      break;
    }
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
    case SI_PIN:
      app_pin_setup();
      break;
    case SI_PINOFF:
      app_pin_remove();
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

// ---------- карусель главного экрана (цветной экран с сенсором) ----------
// Палец тянет ленту монет (menu_touch), отпустил — она доезжает до ближайшей с учётом
// скорости. Касание у края и кнопка листают тем же движением. На OLED ui_dashboard_at()
// не умеет — там кадр меняется сразу.
#define DASH_SLOT 256  // тысячных ширины экрана на одну монету (82 точки из 320)

static bool dragging;
static int drag_from, drag_dx;
static float drag_pos;

static void glide(float from, int to) {
  float d = fabsf((float)to - from);
  if (d < 0.01f) return;
  uint32_t ms = 140 + (uint32_t)(90 * d);
  if (ms > 420) ms = 420;
  for (uint64_t t0 = hal_time_us();;) {
    float t = (float)(hal_time_us() - t0) / 1000.0f / (float)ms;
    if (t >= 1) break;
    float u = 1 - t;
    if (!ui_dashboard_at(from + ((float)to - from) * (1 - u * u * u))) break;  // замедление к концу
    oled_show();
  }
}

void menu_touch(void) {
  int dx, dy;
  if (st != S_DASH || app_locked() || !btn_drag(&dx, &dy)) return;
  if (!dragging) {
    if (abs(dx) * 33 < abs(dy) * 18) return;  // вертикальный свайп — не карусель
    dragging = true;
    drag_from = dash_i;
    drag_dx = dx + 1;
  }
  if (dx == drag_dx) return;
  drag_dx = dx;
  float pos = (float)drag_from - (float)dx / DASH_SLOT;  // лента по кругу: края нет
  if (!ui_dashboard_at(pos)) {
    dragging = false;
    return;
  }
  drag_pos = pos;
  oled_show();
}

// Дальше / назад на главном экране: после свайпа — к монете, куда докатится лента. Лента
// бесконечная: с последней монеты дальше — первая (to уходит за края, номер — по модулю)
static void dash_step(btn_t b) {
  int n = dash_count(), to;
  float from = (float)dash_i;
  if (dragging) {
    float v = -(float)btn_swipe_vx() / DASH_SLOT;  // монет в секунду
    from = drag_pos;
    to = (int)lroundf(drag_pos + v * 0.18f);
    dragging = false;
  } else {
    to = dash_i + (b == BTN_TAP ? 1 : -1);
  }
  glide(from, to);
  dash_i = (to % n + n) % n;
  ui_anim(UI_ANIM_NONE);
  menu_draw();
}

// Назад по списку (свайп вправо на сенсорном экране)
static void menu_prev(void) {
  switch (st) {
    case S_DASH:
      dash_i = (dash_i + dash_count() - 1) % dash_count();
      break;
    case S_APP:
      coin_i = (coin_i + AI_COUNT - 1) % AI_COUNT;
      break;
    case S_BAL:  // назад по страницам, с первой — в меню монеты
      if (bal_page > 0) bal_page--;
      else st = S_APP;
      break;
    case S_ADDR:
      if (addr_page > 0) {
        addr_page--;
      } else {  // первая страница — последняя страница предыдущего адреса
        uint32_t n = (uint32_t)menu_addr_count();
        addr_idx = (addr_idx + n - 1) % n;
        addr_page = ui_receive_pages(address(coin, addr_idx)) - 1;
      }
      break;
    case S_SETTINGS: {
      int items[SI_MAX], n = settings_items(items);
      set_i = (set_i + n - 1) % n;
      break;
    }
  }
}

void menu_event(btn_t b) {
  if (b == BTN_NONE) return;
  if (st == S_DASH && (b == BTN_TAP || b == BTN_PREV)) {
    dash_step(b);
    return;
  }
  dragging = false;
  ui_anim_btn(b);  // листание — сдвигом, выбор — плавной сменой (на цветном экране)
  if (b == BTN_PREV) {
    menu_prev();
    menu_draw();
    return;
  }
  bool tap = b == BTN_TAP;  // BTN_HOLD и BTN_SELECT (касание посередине) — выбрать
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
      } else if (coin_i == AI_BALANCE) {
        balance_t b;
        if (balance_get(coin_app(coin), &b) && b.more) {  // несколько сумм — списком
          st = S_BAL;
          bal_page = 0;
        }
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
    case S_BAL: {  // дальше — следующая страница, с последней (и выбор) — в меню монеты
      balance_t b;
      if (tap && balance_get(coin_app(coin), &b) && bal_page + 1 < ui_balance_pages(b.text)) bal_page++;
      else st = S_APP;
      break;
    }
    case S_ADDR:
      if (!tap) {
        st = S_APP;  // удержание — назад
      } else if (++addr_page >= ui_receive_pages(address(coin, addr_idx))) {
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
