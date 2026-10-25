# smolbase — native ESP-IDF build. Replaces platformio.ini (IDF 6 migration,
# phase 7c); scaffolding proven by spike/idf6.
#
# One firmware image per App (#69), selected at configure time. There is no
# multi-target build: pick the App, get its own build directory.
#
#   idf.py -B build/smolbase    -DSMOLBASE_APP=smolbase    build
#   idf.py -B build/weatherclock -DSMOLBASE_APP=weatherclock build
#   idf.py -B build/gcm         -DSMOLBASE_APP=gcm         build
#
# Or use the checked-in argfiles, which spell those out: `idf.py @smolbase build`.
# CI loops all three so no App can rot — the same guarantee a bare `pio run` gave.
cmake_minimum_required(VERSION 3.16)

# ---- App selection -------------------------------------------------------
# The App names are the ones the release assets and the docs already use. The
# source directories do NOT match them one-for-one (historical), so the mapping
# lives here, in one place, and pack_fs.py carries the same table for html/.
set(SMOLBASE_APP "smolbase" CACHE STRING "App to build: smolbase | weatherclock | gcm")
set_property(CACHE SMOLBASE_APP PROPERTY STRINGS smolbase weatherclock gcm)

set(SB_ROOT ${CMAKE_CURRENT_LIST_DIR})

if(SMOLBASE_APP STREQUAL "smolbase")
  # The stock demo App (Boing clock). Core framebuffer left at its default
  # (8-bpp palette) — this is the only App that uses palette cycling.
  set(SMOLBASE_APP_DIRS
      ${SB_ROOT}/src/app-smolbase
      ${SB_ROOT}/src/app-smolbase/effects
      ${SB_ROOT}/src/app-smolbase/effects/assets)
  set(SMOLBASE_APP_DEFS)
elseif(SMOLBASE_APP STREQUAL "weatherclock")
  set(SMOLBASE_APP_DIRS
      ${SB_ROOT}/src/app-weather
      ${SB_ROOT}/src/app-weather/assets)
  # No core framebuffer (ADR 0004): the weather screen owns a static 240x64
  # RGB565 band scratch and pushes bands directly — full color, ~27 KB less
  # static RAM than the core buffer this build never otherwise used.
  # The asset prefixes point GitHub OTA at this App's own release assets.
  set(SMOLBASE_APP_DEFS
      SMOLBASE_FRAMEBUFFER=0
      "SMOLBASE_FW_ASSET_PREFIX=\"weatherclock-firmware\""
      "SMOLBASE_ASSETS_PREFIX=\"weatherclock-assets\"")
elseif(SMOLBASE_APP STREQUAL "gcm")
  set(SMOLBASE_APP_DIRS ${SB_ROOT}/src/app-gcm)
  # Direct drawing — no core framebuffer; GcmScreen draws straight to the panel
  # via tick(LGFX_Device&), same pattern as the weather app (ADR 0004).
  set(SMOLBASE_APP_DEFS
      SMOLBASE_FRAMEBUFFER=0
      "SMOLBASE_FW_ASSET_PREFIX=\"gcm-firmware\""
      "SMOLBASE_ASSETS_PREFIX=\"gcm-assets\"")
else()
  message(FATAL_ERROR
    "Unknown SMOLBASE_APP='${SMOLBASE_APP}'. Expected one of: smolbase weatherclock gcm")
endif()

# ---- TEMPORARY: upstream patches, applied to the SDK tree ------------------
# Two performance fixes this project measured and sent upstream are applied at
# configure time while their PRs are in review (TF-PSA-Crypto#873 and
# esp-idf#19027). This leaves $IDF_PATH dirty on purpose. When a PR merges,
# delete its patch and restore the SDK: patches/README.md has the protocol,
# and the step fails loudly rather than build a hybrid if the SDK moves.
include(${SB_ROOT}/cmake/upstream_patches.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smolbase)

# ---- Web assets and the filesystem image ---------------------------------
# scripts/pack_fs.py assembles data-<app>/w from html/ plus the App's own
# html/ overlay, gzip-only (PsychicHttp auto-serves name.gz). It used to be a
# PlatformIO extra_script; it is now a plain CLI, called from here and from CI.
add_custom_target(smolbase_assets
  COMMAND ${PYTHON} ${SB_ROOT}/scripts/pack_fs.py --app ${SMOLBASE_APP}
  WORKING_DIRECTORY ${SB_ROOT}
  COMMENT "Packing web assets for ${SMOLBASE_APP}"
  VERBATIM)

# Builds build/<app>/spiffs.bin — the fs-OTA / release image. The partition is
# named "spiffs" for historical reasons (stock flash layout) but holds LittleFS;
# partitions.csv is byte-identical to the shipped one and must stay that way.
#
# The base dir is data-<app>/, NOT data-<app>/w/. `littlefs-python create` lays
# the CONTENTS of its source directory at the image root, so pointing it at w/
# produces an image with /index.html.gz at the top level while the firmware
# serves from /w/ — a valid image that mounts cleanly and answers 404 to
# everything. That flashed with an HTTP 200 and a healthy reboot before anyone
# noticed. PlatformIO's mklittlefs was handed data-<env>/ and so got this right
# by accident. (scripts/pack_fs.py owns the w/ level; the asset tar in CI is
# separately built with -C data-<app>/w because AssetUpdate extracts into /w
# itself.)
littlefs_create_partition_image(spiffs ${SB_ROOT}/data-${SMOLBASE_APP}
                               FLASH_IN_PROJECT DEPENDS smolbase_assets)
