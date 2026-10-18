// channel.h — шифрованный канал для беспроводной связи (BLE)
//
// Транспорт передаёт целые кадры (сам режет их на пакеты и собирает обратно).
// Внутри канала идут те же текстовые команды, что и по USB (app.c).
//
// Кадры (первый байт — тип):
//   01 HELLO      ПК -> устр.: host_pub[32] host_id[8]   (id = нули у нового ПК)
//   02 HELLO_R    устр. -> ПК: dev_pub[32] mode[1]       (0 = сопряжение по коду, 1 = знакомый ПК)
//   03 ERROR      устр. -> ПК: текст
//   10 DATA       обе стороны: ctr[8, BE] шифротекст тег[16]
//
// Ключи: X25519(эфемерные) -> HKDF-SHA256, соль = SHA256("MicroWallet BLE v1" ||
// host_pub || dev_pub || host_id || mode); у знакомого ПК к общему секрету
// добавляется ключ сопряжения. Отдельные ключи на каждое направление,
// ChaCha20-Poly1305 (RFC 7539), nonce = 0x00000000 || ctr, AAD = тип || ctr.
// Счётчик строго растёт: повтор или подмена — разрыв сессии.
//
// Новый ПК: устройство и ПК показывают одинаковый 6-значный код (из той же
// HKDF); пользователь подтверждает его кнопкой. Это защищает от подмены
// устройства посредине. После подтверждения первый DATA от устройства —
// "paired <id hex>", у знакомого ПК — "ready".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHAN_FRAME_MAX 4200

typedef void (*chan_send_fn)(const uint8_t *frame, size_t len);

void chan_reset(void);  // разрыв соединения: забыть сессию
void chan_frame(const uint8_t *frame, size_t len, chan_send_fn send);
bool chan_active(void);

// Хранилище сопряжений — реализует платформа
bool bond_find(const uint8_t id[8], uint8_t key[32]);
bool bond_add(const uint8_t id[8], const uint8_t key[32]);
void bond_forget_all(void);
