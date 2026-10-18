#!/bin/sh
# Внешние исходники в lib/ (зафиксированные версии, только нужные папки):
#   trezor-crypto        — crypto/ из trezor-firmware (MIT)
#   wasm-micro-runtime   — движок WebAssembly для приложений (Apache-2.0 WITH LLVM-exception)
#   sr25519-donna        — sr25519 (schnorrkel, Polkadot) на C (Apache-2.0)
# Уже скачанное не трогается; чтобы обновить — удали папку в lib/.
set -e
TREZOR_COMMIT=94de9b211277b80b28864cd04e6cb6a380a5903a
WAMR_TAG=WAMR-2.4.4
SR25519_COMMIT=e1e8c7df5dd822b402a4ebfc8746303193b9c665
cd "$(dirname "$0")"
mkdir -p lib && cd lib

if [ ! -d trezor-crypto ]; then
  rm -rf trezor-firmware
  git clone --filter=blob:none --no-checkout --depth 1 https://github.com/trezor/trezor-firmware.git
  cd trezor-firmware
  git fetch --depth 1 origin $TREZOR_COMMIT
  git sparse-checkout set crypto
  git checkout -q $TREZOR_COMMIT
  cd ..
  mv trezor-firmware/crypto trezor-crypto
  rm -rf trezor-firmware
  echo "trezor-crypto -> lib/trezor-crypto"
fi

if [ ! -d wasm-micro-runtime ]; then
  git clone --filter=blob:none --no-checkout --depth 1 --branch $WAMR_TAG \
    https://github.com/bytecodealliance/wasm-micro-runtime.git wasm-micro-runtime
  cd wasm-micro-runtime
  git sparse-checkout set core build-scripts
  git checkout -q $WAMR_TAG
  rm -rf .git
  cd ..
  echo "$WAMR_TAG -> lib/wasm-micro-runtime"
fi

if [ ! -d sr25519-donna ]; then
  git clone -q https://github.com/TerenceGe/sr25519-donna.git sr25519-donna
  cd sr25519-donna
  git checkout -q $SR25519_COMMIT
  rm -rf .git
  cd ..
  echo "sr25519-donna -> lib/sr25519-donna"
fi
