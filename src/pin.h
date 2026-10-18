// pin.h — PIN из 4 цифр на одной кнопке: цифра — это длительность нажатия.
//   короче 1 с — 1; дальше +1 за каждые 0.5 с: 1–1.5 с — 2, 1.5–2 с — 3 … 4.5 с и дольше — 9.
// Пока кнопку держат, на экране растёт шкала из 9 делений (самой цифры нет). После 4-й цифры:
// клик — ввести, удержание — набрать заново (неверная попытка не тратится на промах).
//
// PIN защищает доступ, а не seed: 9^4 вариантов перебираются мгновенно, если прочитать флеш.
// От чтения флеша seed защищает шифрование (защищённая сборка), PIN — от того, кто взял
// устройство в руки: после 3 ошибок — паузы 5, 10, 20 … с, после 10-й кошелёк стирается.
// Счётчик ошибок пишется во флеш ДО проверки: выдернуть питание не поможет.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PIN_LEN 4
#define PIN_MAX_FAILS 10

typedef enum { PIN_OK, PIN_WRONG, PIN_WIPED } pin_res_t;

int pin_digit(uint32_t ms);  // длительность нажатия -> цифра 1..9
bool pin_is_set(void);
int pin_fails(void);                    // неверных попыток подряд
uint32_t pin_delay_s(void);             // сколько ждать перед следующей попыткой
bool pin_set(const uint8_t d[PIN_LEN]);
void pin_clear(void);
// Проверить; при PIN_WRONG *left — сколько попыток осталось; PIN_WIPED — кошелёк стёрт
pin_res_t pin_check(const uint8_t d[PIN_LEN], int *left);

// Ввод без блокировки (экран блокировки при включении крутится в app_tick)
typedef struct {
  const char *title;
  uint8_t d[PIN_LEN];
  int n;              // введено цифр
  bool confirm;       // все цифры набраны: клик — готово, удержание — заново
  bool wait_release;  // кнопка ещё нажата с прошлого экрана
  bool down, dirty;
  uint64_t t_down, t_idle;
  int level;          // делений шкалы, пока кнопку держат; -1 — отпущена
} pin_entry_t;

void pin_entry_start(pin_entry_t *e, const char *title);
bool pin_entry_step(pin_entry_t *e);  // true — PIN набран (e->d)
// Ввод с ожиданием (внутри экранов подтверждения). false — минута без нажатий
bool pin_prompt(const char *title, uint8_t d[PIN_LEN]);

#ifdef MW_SIM
// Симулятор: что «набрал человек» — $MW_SIM_PIN или команда «simpin type 1234»
bool sim_pin_typed(uint8_t d[PIN_LEN]);
void sim_pin_type(const char *digits);
#endif
