// ux.h — интерактивные экраны поверх ui.h и кнопки (блокирующие)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  UX_TITLE,   // карусель: иконка, заголовок, подпись
  UX_FIELD,   // заголовок + значение, длинное — на несколько страниц
  UX_AMOUNT,  // заголовок + сумма крупно
  UX_ACCEPT,  // удержание здесь = «да»
  UX_REJECT,  // удержание здесь = «нет»
} ux_kind_t;

typedef struct {
  ux_kind_t kind;
  const uint8_t *icon;
  const char *title;
  const char *text;  // подпись или значение
} ux_step_t;

// Короткое нажатие листает шаги по кругу, удержание на Accept/Reject — выбор.
// Возвращает true, если выбран Accept.
bool ux_review(const ux_step_t *steps, int n);

// Сообщение на ms миллисекунд (0 — до нажатия)
void ux_flash(const uint8_t *icon, const char *title, const char *sub, int ms);
// Ошибка: текст переносится по строкам
void ux_error(const char *msg);
