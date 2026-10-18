#!/bin/bash
# Сборка всего для релиза и (по желанию) публикация на GitHub.
#
#   tools/release.sh                 собрать в build/release/
#   tools/release.sh v0.7.0          собрать и опубликовать релиз v0.7.0 (gh)
#
# Нужны: ESP-IDF 5.4 (~/esp/esp-idf или IDF_PATH), clang с wasm32, JDK 21 + Android SDK,
# gh (вход в аккаунт с правом repo). Защищённые образы подписываются релизным ключом
# MW_RELEASE_KEY (по умолчанию ~/microwallet-keys/release.pem) — его хеш прожжён в платах.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TAG=${1:-}
KEY=${MW_RELEASE_KEY:-$HOME/microwallet-keys/release.pem}
B=$ROOT/build/release-work
OUT=$ROOT/build/release
D=$ROOT/esp32/dist
mkdir -p "$B" "$D"
rm -rf "$OUT"
mkdir -p "$OUT"

[ -f "$KEY" ] || { echo "нет релизного ключа $KEY (MW_RELEASE_KEY)"; exit 1; }
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1

# ---- приложения ----
sh apps/build.sh >/dev/null
python3 tools/mkapps.py "$D/microwallet-esp32s3-apps.bin" >/dev/null

# ---- прошивки: обычные (без подписи) и защищённые (релизный ключ) ----
printf 'CONFIG_SECURE_BOOT_SIGNING_KEY="%s"\n' "$KEY" > "$B/key.cfg"
build() {  # каталог, defaults, имя
  idf.py -C esp32 -B "$B/$1" -D SDKCONFIG="$B/$1.sdkconfig" -D "SDKCONFIG_DEFAULTS=$2" build > "$B/$1.log" 2>&1 \
    || { tail -30 "$B/$1.log"; exit 1; }
  cp "$B/$1/microwallet.bin" "$OUT/microwallet-esp32s3-$3-app.bin"
}
build t "sdkconfig.defaults" testnet
build m "sdkconfig.defaults;sdkconfig.mainnet" mainnet
build ts "sdkconfig.defaults;sdkconfig.secure;$B/key.cfg" testnet-secure
build ms "sdkconfig.defaults;sdkconfig.mainnet;sdkconfig.secure;$B/key.cfg" mainnet-secure
# Загрузчик ESP-IDF доверяет ключу из слота eFuse №k, только если у образа есть блок
# подписи №k (secure_boot_signatures_bootloader.c): релизный ключ — в слоте 1, поэтому
# образу нужны блоки 0 и 1. Подписываем релизным ключом дважды — иначе обновление
# запишется, а плата загрузит прежнюю прошивку.
for n in testnet-secure mainnet-secure; do
  f="$OUT/microwallet-esp32s3-$n-app.bin"
  espsecure.py sign_data --version 2 --append_signatures --keyfile "$KEY" -o "$f.2" "$f" >/dev/null
  mv "$f.2" "$f"
  [ "$(espsecure.py verify_signature --version 2 --keyfile "$KEY" "$f" 2>&1 | grep -c "verification successful")" = 2 ] \
    || { echo "$n: нужны две подписи релизным ключом"; exit 1; }
done
for v in "t testnet" "m mainnet"; do
  set -- $v
  (cd "$B/$1" && esptool.py --chip esp32s3 merge_bin -o "$OUT/microwallet-esp32s3-$2-full.bin" \
    --flash_mode dio --flash_size 4MB @flash_args 0x280000 "$D/microwallet-esp32s3-apps.bin" >/dev/null)
done
cp "$D/microwallet-esp32s3-apps.bin" "$D/microwallet-esp32s3-partitions.bin" "$D/microwallet-esp32s3-otadata.bin" "$OUT/"
cp apps/dist/microwallet-app-*.wasm "$OUT/"

# ---- приложение для телефона ----
(cd android && JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk} ./gradlew -q :app:assembleRelease)
python3 tools/android_dist.py >/dev/null
cp android/dist/microwallet-android.apk android/dist/microwallet-android.json "$OUT/"

(cd "$OUT" && sha256sum -- * > SHA256SUMS)
ls -la "$OUT"

if [ -n "$TAG" ]; then
  NOTES=${NOTES:-$ROOT/build/release-notes.md}
  [ -f "$NOTES" ] || { echo "нет описания релиза $NOTES (NOTES=…)"; exit 1; }
  gh release create "$TAG" --repo delarun/MicroWallet --target main --title "MicroWallet $TAG" \
    --notes-file "$NOTES" "$OUT"/*
fi
