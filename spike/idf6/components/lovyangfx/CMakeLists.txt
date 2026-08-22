# OUR wrapper around an upstream LovyanGFX checkout — not upstream's CMakeLists.
#
# Upstream's root CMakeLists.txt -> boards.cmake/esp-idf.cmake still uses the
# pre-v4 register_component() / COMPONENT_SRCS / COMPONENT_ADD_INCLUDEDIRS
# style. That has been carried by an IDF compatibility layer for years, and
# whether IDF 6 still carries it is exactly the kind of thing that breaks
# quietly. This does the same glob through modern idf_component_register(), so
# the spike does not depend on the answer.
#
# The glob list and the REQUIRES list are copied from upstream's
# boards.cmake/esp-idf.cmake (LovyanGFX 1.2.27), IDF_VERSION_MAJOR >= 6 branch.
# The platforms/arduino_default/*.cpp files compile to nothing when ESP_PLATFORM
# is defined (empty first branch of their #if) — globbed to match upstream.
#
# Clone the upstream tree first (gitignored):
#   git clone --depth 1 --branch 1.2.27 \
#     https://github.com/lovyan03/LovyanGFX.git components/lovyangfx/upstream

set(LGFX_ROOT ${CMAKE_CURRENT_LIST_DIR}/upstream)

if(NOT EXISTS ${LGFX_ROOT}/src/LovyanGFX.hpp)
  message(FATAL_ERROR
    "LovyanGFX upstream checkout missing. Run:\n"
    "  git clone --depth 1 --branch 1.2.27 "
    "https://github.com/lovyan03/LovyanGFX.git components/lovyangfx/upstream")
endif()

file(GLOB LGFX_SRCS
  ${LGFX_ROOT}/src/lgfx/Fonts/efont/*.c
  ${LGFX_ROOT}/src/lgfx/Fonts/IPA/*.c
  ${LGFX_ROOT}/src/lgfx/Fonts/lvgl/*.c
  ${LGFX_ROOT}/src/lgfx/utility/*.c
  ${LGFX_ROOT}/src/lgfx/v1/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/lv_font/*.c
  ${LGFX_ROOT}/src/lgfx/v1/misc/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/panel/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/platforms/arduino_default/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/platforms/esp32/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/touch/*.cpp
)

idf_component_register(
  SRCS ${LGFX_SRCS}
  INCLUDE_DIRS ${LGFX_ROOT}/src
  REQUIRES nvs_flash efuse esp_lcd driver esp_timer esp_mm esp_driver_ledc
           esp_driver_i2s hal
)

# Upstream is third-party and not warning-clean under our flags; do not let it
# fail the build we are trying to measure.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function)
