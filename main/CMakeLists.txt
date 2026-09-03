# The whole firmware is this one component. Sources live in ../src (they
# predate the CMake build and the layout is documented in AGENTS.md); only the
# manifest lives here, because that is where IDF insists the "main" component's
# CMakeLists.txt sits.
#
# SMOLBASE_APP_DIRS / SMOLBASE_APP_DEFS come from the root CMakeLists.txt and
# are absolute paths. src/core/ and src/main.cpp always compile; exactly one
# src/app-*/ provides makeApp() — two would be a duplicate-symbol link error,
# zero a missing-symbol one, both by design.
#
# SRC_DIRS only, no SRCS: IDF ignores SRC_DIRS entirely when both are given
# (with a warning that is easy to miss in a long configure log). ../src is
# listed for main.cpp, which is the only source that lives directly in it.
idf_component_register(
  SRC_DIRS "${SB_ROOT}/src" "${SB_ROOT}/src/core" ${SMOLBASE_APP_DIRS}
  # ../src for main.cpp's "core/Foo.h"; ../src/core for core's own flat
  # includes; ../include for smolbase_config.h; the App dirs for their
  # "assets/..." and "effects/..." spellings.
  INCLUDE_DIRS
    "${SB_ROOT}/include"
    "${SB_ROOT}/src"
    "${SB_ROOT}/src/core"
    ${SMOLBASE_APP_DIRS}
  REQUIRES
    # Public: these appear in our headers (Net.h, Http.h, Web.h, ...).
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp_http_server
    esp_https_ota
    esp-tls
    mbedtls
    nvs_flash
    app_update
    esp_app_format
    esp_partition
    esp_timer
    esp_driver_touch_sens
    spi_flash
    lwip
    espressif__mdns
    joltwallet__littlefs
    psychichttp
    bblanchon__arduinojson
    lovyangfx
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

if(SMOLBASE_APP_DEFS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${SMOLBASE_APP_DEFS})
endif()
