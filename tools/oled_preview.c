// Симулятор экрана: рисует экраны через src/ui.c и сохраняет кадры для oled_preview.py
//   gcc -Isrc tools/oled_preview.c src/gfx.c src/ui.c src/icons.c -o preview            (128x64)
//   gcc -Isrc -DGFX_H=32 tools/oled_preview.c src/gfx.c src/ui.c src/icons.c -o preview (128x32)
//   ./preview out/ && python3 tools/oled_preview.py out/ screens.png
// Цветной интерфейс Waveshare 1.47 (src/ui_lcd.c, 320x172):
//   gcc -Isrc -Ilib/qrcodegen -DUI_LCD tools/oled_preview.c src/ui_lcd.c src/draw.c src/fonts_lcd.c
//       src/icons.c lib/qrcodegen/qrcodegen.c -lm -o preview
//   ./preview out/ && python3 tools/oled_preview.py out/ screens.png
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "coins.h"
#include "gfx.h"
#include "icons.h"
#include "ui.h"

static const char *dir;
static int n;

#ifdef UI_LCD
#include "draw.h"

static void save(const char *label) {  // кадр PPM + подпись
  char path[512];
  snprintf(path, sizeof path, "%s/%02d.ppm", dir, n);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", LCD_W, LCD_H);
  for (int i = 0; i < LCD_W * LCD_H; i++) {
    uint16_t c = lcd_fb[i];
    fputc((c >> 11) * 255 / 31, f);
    fputc((c >> 5 & 63) * 255 / 63, f);
    fputc((c & 31) * 255 / 31, f);
  }
  fclose(f);
  snprintf(path, sizeof path, "%s/%02d.txt", dir, n++);
  f = fopen(path, "wb");
  fwrite(label, 1, strlen(label), f);
  fclose(f);
}
#else
static void save(const char *label) {
  char path[512];
  snprintf(path, sizeof path, "%s/%02d.bin", dir, n++);
  FILE *f = fopen(path, "wb");
  fputc(GFX_H, f);  // высота экрана: 32 или 64
  fwrite(gfx_fb, 1, sizeof gfx_fb, f);
  fwrite(label, 1, strlen(label), f);
  fclose(f);
}
#endif

int main(int argc, char **argv) {
  dir = argc > 1 ? argv[1] : ".";
#ifdef UI_LCD
  draw_init();
#endif
  const char *addr = "tprl1pr6yuq8u2r95wjzzgpdy8cpnncpl7l8zgy6x5q0367pnc53s2famqg7pt74";

  ui_splash(FW_VERSION, "testnet");                                     save("Заставка");
  const uint8_t *dash[] = {ICON_BTC, ICON_PRL, ICON_TRON, ICON_BSC, ICON_GEAR};
  const char *names[] = {"Bitcoin", "Pearl", "TRON", "BNB Chain", "Settings"};
  ui_dashboard(dash, names, 5, 0, "Bitcoin", "BTC  mainnet");
  ui_status("mainnet", true, false, true, 72);                       save("Главный экран: Bitcoin");
  ui_dashboard(dash, names, 5, 1, "Pearl", "PRL  mainnet");
  ui_status("mainnet", true, true, false, 18);                       save("Главный экран: Pearl (BLE подключён)");
  ui_dashboard(dash, names, 5, 2, "TRON", "TRX  USDT  mainnet");           save("Главный экран: TRON");
  ui_dashboard(dash, names, 5, 3, "BNB Chain", "BNB  USDT  mainnet");      save("Главный экран: BNB Chain");
  ui_dashboard(dash, names, 5, 4, "Settings", "brightness, wipe");         save("Главный экран: Settings");
  ui_carousel(ICON_RECEIVE, "Receive", "Show address", 0, 3);      save("Pearl: получить");
  ui_carousel(ICON_INFO, "Info", "m/86'/1'/0'", 1, 3);             save("Pearl: инфо");
  ui_carousel(ICON_BACK, "Back", NULL, 2, 3);                      save("Pearl: назад");
  ui_receive(ICON_PRL, "Pearl #0", addr, 0, ui_receive_pages(addr));  save("Получить: Pearl");
  ui_receive(ICON_BTC, "Bitcoin #3", "tb1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu", 0, 1);  save("Получить: Bitcoin");
  ui_receive(ICON_COIN, "TON #0", "0QBmSEEMnOtOU7DR8Uh0OHJEmXb6jaN7WyAJsdBvxbG8hc-z", 0, 1);  save("Получить: TON");
  ui_field(ICON_PRL, "Pearl #0", addr, 0, ui_pages(addr), -1, 0);  save("Адрес, стр. 1");
  if (ui_pages(addr) > 1) {
    ui_field(ICON_PRL, "Pearl #0", addr, 1, ui_pages(addr), -1, 0);
    save("Адрес, стр. 2");
  }
  ui_carousel(ICON_EYE, "Verify", "address", 0, 5);                save("Проверка адреса с ПК");
  ui_carousel(ICON_CHECK, "Approve", "hold if it matches", 3, 5);  save("Проверка: подтвердить");
  ui_carousel(ICON_EYE, "Review", "Pearl transfer", 0, 7);         save("Подпись: начало");
  ui_amount(ICON_COIN, "Amount", "12.5 tPRL", 1, 7);               save("Подпись: сумма");
  ui_field(ICON_SEND, "To", addr, 0, ui_pages(addr), 2, 7);                     save("Подпись: получатель");
  ui_field(ICON_FEE, "Fee", "0.00000424 tPRL", 0, 1, 4, 7);         save("Подпись: комиссия (длинная)");
  ui_amount(ICON_COIN, "Amount", "250 USDT", 1, 7);                save("TRON: сумма");
  ui_carousel(ICON_CHECK, "Accept", "hold to sign", 5, 7);         save("Подпись: принять");
  ui_carousel(ICON_CROSS, "Reject", "hold to cancel", 6, 7);       save("Подпись: отклонить");
  ui_carousel(ICON_WARN, "High fee!", "check it carefully", 4, 7); save("Предупреждение");
  ui_progress(ICON_PEN, "Signing", 60);                            save("Подписываю");
  ui_progress(ICON_LOCK, "Unlocking", 35);                         save("Разблокировка");
  ui_message(ICON_CHECK, "Signed", NULL);                          save("Готово");
  ui_word("Word 3 of 12", "abandon", 2, 12);                       save("Мнемоника");
  ui_word("Word #5 is...?", "category", 1, 3);                     save("Проверка слова");
  ui_pin("Enter PIN", 0, -1, false);                               save("PIN: начало");
  ui_pin("Enter PIN", 2, 5, false);                                save("PIN: держу кнопку");
  ui_pin("Enter PIN", 4, -1, true);                                save("PIN: ввести?");
  ui_message(ICON_CROSS, "Wrong PIN", "8 attempts left");          save("PIN: неверный");
  ui_message(ICON_LOCK, "Too many tries", "wait 20 s");            save("PIN: пауза");
  ui_carousel(ICON_SUN, "Brightness", "level 3 of 4", 0, 5);       save("Настройки: яркость");
  ui_carousel(ICON_MOON, "Sleep", "after 1 min", 1, 5);            save("Настройки: сон");
  ui_carousel(ICON_BATT, "Battery", "3.92 V  65%", 2, 8);          save("Настройки: аккумулятор");
  ui_carousel(ICON_TRASH, "Wipe device", "erase the seed", 3, 5);  save("Настройки: сброс");
  const uint8_t *empty[] = {ICON_PLUS, ICON_GEAR};
  const char *empty_names[] = {"New wallet", "Settings"};
  ui_dashboard(empty, empty_names, 2, 0, "New wallet", "hold to create");       save("Нет кошелька");
  ui_carousel(ICON_BT, "Pair with PC?", "Bluetooth", 0, 4);        save("BLE: сопряжение");
  ui_amount(ICON_KEY, "Code", "761 828", 1, 4);                    save("BLE: код");
  ui_carousel(ICON_CHECK, "Pair", "if codes match", 2, 4);         save("BLE: подтвердить");
  ui_carousel(ICON_KEY, "Restore", "seed from PC", 0, 5);          save("Восстановление с ПК");
  ui_field(ICON_PEN, "Seed", "12 words: abandon ... about", 0, 1, 1, 5); save("Восстановление: слова");
  ui_carousel(ICON_WARN, "PC saw seed", "trust this PC?", 2, 5); save("Восстановление: предупреждение");
  ui_carousel(ICON_BT, "Bluetooth", "on, connected", 2, 7);        save("Настройки: Bluetooth");
  ui_carousel(ICON_TRASH, "Forget PCs", "remove BLE pairings", 3, 7); save("Настройки: забыть ПК");
  ui_field(ICON_KEY, "SHA-256", "97a2f55748a81b905dfbcfa775655b8b0c51f3accd60c7f525d7f9cb35130118", 0, 1, 2, 5);
  save("Обновление: хеш");
  ui_progress(ICON_SEND, "Updating", 45);                          save("Обновление: прогресс");
  return 0;
}
