// button.h — единственная кнопка ввода (GPIO1 -> GND, BOOT параллельно)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { BTN_NONE = 0, BTN_TAP, BTN_HOLD } btn_t;

#define BTN_HOLD_MS 750

void btn_init(void);
bool btn_pressed(void);
btn_t btn_poll(void);     // неблокирующий, звать каждые ~10 мс
btn_t btn_wait(void);     // ждать событие
uint32_t btn_last_press_us(void);  // длительность последнего нажатия (для энтропии)
