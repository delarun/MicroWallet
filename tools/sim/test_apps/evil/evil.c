// Тестовое приложение: проверяет, что песочница не даёт лишнего.
// Команды: steal (ключ чужого пути), sign (подпись без подтверждения),
//          oob (указатель за пределы памяти), twice (ответ два раза), trap, own,
//          loop (бесконечный цикл), loopcall (бесконечный цикл с вызовами прошивки)
#include "mw.h"

void mw_main(const char *cmd, size_t len) {
  uint8_t out[65], h[32] = {1};
  (void)len;
  if (!memcmp(cmd, "steal", 5)) {
    uint32_t btc[5] = {H(84), H(0), H(0), 0, 0};
    mw_ok(mw_pubkey(btc, 5, out) == -2 ? "denied" : "LEAK");
  } else if (!memcmp(cmd, "sign", 4)) {
    uint32_t own[5] = {H(44), H(9999), H(0), 0, 0};
    mw_ok(mw_sign(own, 5, h, out) == -4 ? "denied" : "SIGNED WITHOUT CONFIRM");
  } else if (!memcmp(cmd, "own", 3)) {
    uint32_t own[5] = {H(44), H(9999), H(0), 0, 0};
    mw_ok(mw_pubkey(own, 5, out) == 0 ? "allowed" : "broken");
  } else if (!memcmp(cmd, "oob", 3)) {
    mw_reply((const char *)0x7FFFFFF0, 64);  // за пределами 64 КБ памяти модуля
  } else if (!memcmp(cmd, "twice", 5)) {
    mw_ok("first");
    mw_ok("second");
  } else if (!memcmp(cmd, "edsteal", 7)) {  // ed25519 по чужому пути (Solana)
    uint32_t sol[4] = {H(44), H(501), H(0), H(0)};
    mw_ok(mw_ed25519_pubkey(sol, 4, out) == -2 ? "denied" : "LEAK");
  } else if (!memcmp(cmd, "srsteal", 7)) {  // sr25519 без sr25519=yes в манифесте
    mw_ok(mw_sr25519_pubkey(h, 0, out) == -2 ? "denied" : "LEAK");
  } else if (!memcmp(cmd, "trap", 4)) {
    __builtin_trap();
  } else if (!memcmp(cmd, "loopcall", 8)) {
    for (volatile int i = 0;; i++) mw_net();
  } else if (!memcmp(cmd, "loop", 4)) {
    for (volatile int i = 0;; i++) {
    }
  } else {
    mw_ok("hello");
  }
}
