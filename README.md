# smolbase

[![build](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml/badge.svg)](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml)

Template firmware for the GeekMagic **Small TV Pro** (ESP32, ST7789 240×240): WiFi
provisioning via captive portal, settings web UI, NTP + timezones, OTA, LittleFS
assets, and a small extension surface to build your own firmware on.

**Status: scaffold.** It builds and boots; the build slices are being worked through
the [wayfinder map](https://github.com/sweetlilmre/smolbase/issues/1).

## Build

```
pio run            # firmware
pio run -t buildfs # LittleFS image (packs html/ as gzip-only assets)
```

Requires [PlatformIO](https://platformio.org/). The platform pin (pioarduino,
arduino-esp32 3.3.x / IDF 5.5) and exact library pins live in `platformio.ini`.

## Where your code goes

`src/app/` is yours — it ships with the Stock Screen as a worked example. `src/core/`
is plumbing. The contract between them is `src/core/App.h`; the vocabulary is in
[CONTEXT.md](CONTEXT.md), decisions in [docs/adr/](docs/adr/) and on the wayfinder map.
