// apps.h — приложения на WebAssembly (движок WAMR).
//
// Приложение — модуль .wasm с пользовательской секцией "microwallet" (манифест):
//   id=eth            2..8 символов [a-z0-9]: команды "app eth ..."
//   name=Ethereum
//   version=1.0
//   icon=3c7effff7e3c1800   иконка 8x8: 8 байт-строк, старший бит слева
//   path=44'/60'      разрешённый префикс пути BIP32/SLIP-10 (до 4 строк path=) — для
//                     secp256k1 и ed25519
//   sr25519=yes       доступ к ключам sr25519 (Substrate: Polkadot, Kusama …)
//
// Модуль видит только свою память. Ключи, экран и кнопка — через функции прошивки
// (модуль "mw", sdk/mw.h). Прошивка проверяет: ключи — только по путям из манифеста,
// подпись — только после того, как человек подтвердил экран проверки в этом же вызове.
//
// Экспорты модуля:
//   mw_buf() -> адрес буфера (4 КБ) для строки команды
//   mw_cmd(len) — выполнить команду; ответ — через reply() или fail()
// Соглашение для монет: "addr <i>" -> адрес #i (его показывает меню устройства).
//
// Протокол (строки по USB или BLE):
//   apps                                   -> ok id|версия|имя;...
//   app add <размер> <sha256>              установка: затем app add b64 <...>, app add end
//   app add end                            -> экран проверки (имя, версия, пути, хеш)
//   app rm <id>                            -> удаление с подтверждением
//   app <id> <команда приложения>
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#define APP_PATHS 4
#define APP_PATH_DEPTH 5

typedef struct {
  uint32_t off, span;  // где лежит в разделе apps (заголовок + модуль, с округлением до сектора)
  uint32_t seq;        // поколение: при обновлении новая копия пишется раньше, чем стирается старая
  bool sr25519;        // манифест: sr25519=yes — доступ к ключам sr25519 (Substrate)
  char id[9];
  char name[17];
  char version[9];
  uint8_t icon[8];
  int npaths;
  uint8_t path_len[APP_PATHS];
  uint32_t paths[APP_PATHS][APP_PATH_DEPTH];
} mwapp_t;

void apps_init(void);  // прочитать установленные приложения
int apps_count(void);
const mwapp_t *apps_get(int i);

// Команды apps / app ... (true — строка обработана, ответ отправлен)
bool apps_line(char *line, app_reply_fn reply);

// Выполнить команду приложения и вернуть текст ответа (для меню устройства)
bool apps_query(const mwapp_t *a, const char *cmd, char *out, int outlen);

// Удалить с подтверждением на экране (из меню устройства)
bool apps_remove(const mwapp_t *a);

// Пути манифеста одной строкой: "44'/60', 44'/1'" (+ ", sr25519")
void apps_paths_str(const mwapp_t *a, char *out, int outlen);

// Раздел apps: всего и свободно, байт
uint32_t apps_total(void);
uint32_t apps_free(void);
