#!/bin/sh
# Сборка приложения MicroWallet: sdk/build.sh apps/eth/eth.c build/eth.wasm
#   манифест — файл manifest рядом с исходником, дописывается секцией "microwallet"
#   -mcpu=mvp: только базовый WebAssembly 1.0 — его понимает любой движок
#   память модуля — одна страница 64 КБ (стек 16 КБ), без libc
set -e
SDK="$(cd "$(dirname "$0")" && pwd)"
SRC="$1"
OUT="${2:-${SRC%.c}.wasm}"
[ -n "$SRC" ] || { echo "usage: $0 app.c [out.wasm]"; exit 1; }
mkdir -p "$(dirname "$OUT")"
clang --target=wasm32 -mcpu=mvp -O2 -nostdlib -ffreestanding -fno-builtin \
  -Wall -Wextra -Werror -I"$SDK" \
  -Wl,--no-entry -Wl,--strip-debug \
  -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=16384 \
  -o "$OUT.tmp" "$SRC" "$SDK/mw.c"
python3 "$SDK/pack.py" "$OUT.tmp" "$(dirname "$SRC")/manifest" "$OUT"
rm -f "$OUT.tmp"
echo "-> $OUT ($(wc -c < "$OUT") байт)"
