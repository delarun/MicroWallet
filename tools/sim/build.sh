#!/bin/sh
# Сборка симулятора: tools/sim/build.sh [выходной файл]
set -e
cd "$(dirname "$0")/../.."
TC=lib/trezor-crypto
WAMR=lib/wasm-micro-runtime
OUT=${1:-mwsim}
# Движок WebAssembly (WAMR) — статической библиотекой, один раз
if [ ! -f build/sim-wamr/libvmlib.a ] || [ tools/sim/wamr/CMakeLists.txt -nt build/sim-wamr/libvmlib.a ]; then
  cmake -S tools/sim/wamr -B build/sim-wamr -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build/sim-wamr -j >/dev/null
fi
# sr25519 (lib/sr25519-donna) — отдельно: свои заголовки и префикс srd_ для глобальных имён
SR=lib/sr25519-donna/src
mkdir -p build/sim-sr25519
for f in sr25519 ristretto255 merlin vrf sha2; do
  gcc -O2 -w -Wno-incompatible-pointer-types -c -include src/sr25519_prefix.h -I$SR -DED25519_CUSTOMRANDOM -DED25519_CUSTOMHASH \
    -DED25519_NO_INLINE_ASM $SR/$f.c -o build/sim-sr25519/$f.o
done
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -I$TC -I$WAMR/core/iwasm/include -DMW_SIM=1 -DGFX_H=64 \
  -DUSE_PRECOMPUTED_CP=1 -DUSE_BIP39_CACHE=0 ${MW_SIM_CFLAGS} \
  tools/sim/sim_main.c tools/sim/hal_sim.c \
  src/app.c src/channel.c src/menu.c src/ux.c src/ui.c src/gfx.c src/icons.c src/oled.c \
  src/button.c src/wallet.c src/schnorr.c src/apps.c src/sr25519_glue.c \
  -w $(for f in address base58 bignum bip39 bip39_english blake256 blake2b groestl \
    hasher hmac hmac_drbg memzero pbkdf2 rfc6979 ripemd160 secp256k1 ecdsa \
    segwit_addr sha2 sha3 consteq \
    ed25519-donna/curve25519-donna-32bit ed25519-donna/curve25519-donna-helpers \
    ed25519-donna/modm-donna-32bit ed25519-donna/ed25519-donna-basepoint-table \
    ed25519-donna/ed25519-donna-32bit-tables ed25519-donna/ed25519-donna-impl-base \
    ed25519-donna/ed25519 ed25519-donna/curve25519-donna-scalarmult-base \
    chacha20poly1305/chacha20poly1305 chacha20poly1305/chacha_merged \
    chacha20poly1305/poly1305-donna chacha20poly1305/rfc7539; do echo $TC/$f.c; done) \
  build/sim-sr25519/*.o build/sim-wamr/libvmlib.a -lm -lpthread \
  -o "$OUT"
echo "-> $OUT"
