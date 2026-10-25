# OUR wrapper around an upstream LovyanGFX checkout — not upstream's CMakeLists.
#
# Upstream's root CMakeLists.txt -> boards.cmake/esp-idf.cmake still uses the
# pre-v4 register_component() / COMPONENT_SRCS / COMPONENT_ADD_INCLUDEDIRS
# style, carried for years by an IDF compatibility layer. Rather than depend on
# whether IDF 6 still carries it, this does the same glob through modern
# idf_component_register(). Proven by spike/idf6 on this hardware.
#
# LovyanGFX is not in the ESP component registry, so it cannot be an
# idf_component.yml dependency; the checkout is cloned here at configure time
# from the pinned tag (gitignored). The glob list and the REQUIRES list are
# copied from upstream's boards.cmake/esp-idf.cmake, IDF_VERSION_MAJOR >= 6
# branch. platforms/arduino_default/*.cpp compile to nothing when ESP_PLATFORM
# is defined (empty first branch of their #if) — globbed to match upstream.
#
# The version is an EXACT tag, matching the old lib_deps pin, for template
# reproducibility (wayfinder #3, #4). Upstream tags carry no "v" prefix.

set(LGFX_VERSION 1.2.27)
set(LGFX_ROOT ${CMAKE_CURRENT_LIST_DIR}/upstream)

if(NOT EXISTS ${LGFX_ROOT}/src/LovyanGFX.hpp)
  find_package(Git REQUIRED)
  message(STATUS "Cloning LovyanGFX ${LGFX_VERSION} into ${LGFX_ROOT}")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch ${LGFX_VERSION}
            https://github.com/lovyan03/LovyanGFX.git ${LGFX_ROOT}
    RESULT_VARIABLE lgfx_clone_result)
  if(NOT lgfx_clone_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to clone LovyanGFX ${LGFX_VERSION}. Clone it by hand and re-run:\n"
      "  git clone --depth 1 --branch ${LGFX_VERSION} "
      "https://github.com/lovyan03/LovyanGFX.git ${LGFX_ROOT}")
  endif()
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
# fail our build.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function)
