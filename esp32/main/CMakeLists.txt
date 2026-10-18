# Ядро кошелька — общие исходники из ../../src, криптография — trezor-crypto
set(PW ${CMAKE_CURRENT_LIST_DIR}/../..)
set(TC ${PW}/lib/trezor-crypto)

set(CORE app apps channel menu ux ui gfx icons oled button pin wallet schnorr sr25519_glue)
list(TRANSFORM CORE PREPEND ${PW}/src/)
list(TRANSFORM CORE APPEND .c)

set(TC_FILES
  address base58 bignum bip39 bip39_english blake256 blake2b consteq ecdsa
  groestl hasher hmac hmac_drbg memzero pbkdf2 rfc6979 ripemd160 secp256k1
  segwit_addr sha2 sha3
  ed25519-donna/curve25519-donna-32bit ed25519-donna/curve25519-donna-helpers
  ed25519-donna/modm-donna-32bit ed25519-donna/ed25519-donna-basepoint-table
  ed25519-donna/ed25519-donna-32bit-tables ed25519-donna/ed25519-donna-impl-base
  ed25519-donna/ed25519 ed25519-donna/curve25519-donna-scalarmult-base
  chacha20poly1305/chacha20poly1305 chacha20poly1305/chacha_merged
  chacha20poly1305/poly1305-donna chacha20poly1305/rfc7539)
list(TRANSFORM TC_FILES PREPEND ${TC}/)
list(TRANSFORM TC_FILES APPEND .c)

# sr25519 (schnorrkel, Substrate) — lib/sr25519-donna: свои заголовки и префикс srd_ для
# глобальных имён (src/sr25519_prefix.h), чтобы не столкнуться с trezor-crypto
set(SR ${PW}/lib/sr25519-donna/src)
set(SR_FILES ${SR}/sr25519.c ${SR}/ristretto255.c ${SR}/merlin.c ${SR}/vrf.c ${SR}/sha2.c)

idf_component_register(
  SRCS main.c hal_esp.c storage_esp.c ble.c ota.c ${CORE} ${TC_FILES} ${SR_FILES}
  INCLUDE_DIRS . ${PW}/src ${TC}
  REQUIRES bt nvs_flash esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag
           bootloader_support esp_timer esp_hw_support app_update esp_adc esp_pm esp_partition
           wamr)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  GFX_H=64
  USE_PRECOMPUTED_CP=1 USE_BIP32_CACHE=0 USE_BIP39_CACHE=0
  USE_ETHEREUM=0 USE_GRAPHENE=0 USE_NEM=0 USE_MONERO=0 USE_CARDANO=0
  USE_BIP32_25519_CURVES=0)
if(CONFIG_MW_MAINNET)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE MW_MAINNET=1)
endif()
# trezor-crypto собирается без наших предупреждений
set_source_files_properties(${TC_FILES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${SR_FILES} PROPERTIES
  INCLUDE_DIRECTORIES "${SR}"
  COMPILE_OPTIONS "-include;${PW}/src/sr25519_prefix.h;-w;-Wno-incompatible-pointer-types"
  COMPILE_DEFINITIONS "ED25519_CUSTOMRANDOM;ED25519_CUSTOMHASH;ED25519_NO_INLINE_ASM")
