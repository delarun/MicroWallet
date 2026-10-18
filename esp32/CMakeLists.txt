# MicroWallet для ESP32-S3 SuperMini (ESP-IDF 5.4+)
#   idf.py build flash         (цель esp32s3 задана ниже)
cmake_minimum_required(VERSION 3.16)
# Версия в заголовке образа — та же, что показывает устройство (FW_VERSION в src/coins.h)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../src/coins.h" _fw REGEX "define FW_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" _ "${_fw}")
set(PROJECT_VER "${CMAKE_MATCH_1}")
# coins.h изменился — перенастроить: иначе в заголовке образа останется старая версия
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../src/coins.h")
# Плата одна — ESP32-S3, «idf.py set-target» не нужен
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
  set(IDF_TARGET "esp32s3")
endif()
# Движок WebAssembly для приложений (get_deps.sh скачивает его в lib/)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../lib/wasm-micro-runtime/build-scripts/esp-idf)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Лимит инструкций приложений (src/apps.c): в Kconfig компонента WAMR такой опции нет
idf_build_set_property(COMPILE_DEFINITIONS "WASM_ENABLE_INSTRUCTION_METERING=1" APPEND)
project(microwallet)
