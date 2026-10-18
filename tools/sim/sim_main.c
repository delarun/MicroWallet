// Симулятор MicroWallet на ПК (ядро src/ + канал), для тестов хоста без железа.
//   mwsim lines   — построчный протокол, как по USB
//   mwsim frames  — кадры шифрованного канала: [длина 2 байта BE][кадр], как по BLE
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "channel.h"

static void line_reply(const char *s) {
  printf("%s\n", s);
  fflush(stdout);
}

static void frame_send(const uint8_t *f, size_t n) {
  uint8_t h[2] = {(uint8_t)(n >> 8), (uint8_t)n};
  fwrite(h, 1, 2, stdout);
  fwrite(f, 1, n, stdout);
  fflush(stdout);
}

int main(int argc, char **argv) {
  static char line[APP_LINE_MAX];
  static uint8_t frame[CHAN_FRAME_MAX];
  app_init();
  if (argc > 1 && !strcmp(argv[1], "frames")) {
    uint8_t h[2];
    while (fread(h, 1, 2, stdin) == 2) {
      size_t n = (size_t)h[0] << 8 | h[1];
      if (n > sizeof frame || fread(frame, 1, n, stdin) != n) break;
      chan_frame(frame, n, frame_send);
    }
    return 0;
  }
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    app_line(line, line_reply);
  }
  return 0;
}
