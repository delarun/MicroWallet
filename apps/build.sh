#!/bin/sh
# Собрать все приложения: apps/<id>/<id>.c + manifest -> apps/dist/microwallet-app-<id>-<версия>.wasm
# Эти файлы кладутся в релиз на GitHub; tools/fw_server.py раздаёт их телефону с ПК.
set -e
cd "$(dirname "$0")"
mkdir -p dist
for d in */; do
  d=${d%/}
  [ -f "$d/manifest" ] || continue
  id=$(sed -n 's/^id=//p' "$d/manifest")
  ver=$(sed -n 's/^version=//p' "$d/manifest")
  rm -f dist/microwallet-app-"$id"-*.wasm
  sh ../sdk/build.sh "$d/$id.c" "dist/microwallet-app-$id-$ver.wasm"
done
