---
id: 1
title: Charter grilling — destination and baseline decisions
labels: [wayfinder:grilling]
status: closed
assignee: petere
blocked-by: []
---

## Question

What is this effort's destination, scope, and baseline technical shape? (Two-round grilling session, 2026-08-08, from the brief in `info.md`.)

## Resolution

**Destination**: the MVP template firmware **smolbase** builds, flashes, and demonstrates the full first-run → configured lifecycle in this repo. Execution is carried into the map (override of wayfinder's plan-only default).

**Scope**: all seven features from `info.md` **plus OTA** (firmware + filesystem upload — essential for the dev loop).

**Decisions**:

1. **Consumption model**: GitHub *template repository* — consumers clone and modify; architecture optimizes for an obvious, small extension surface rather than a sealed library API.
2. **Build system**: PlatformIO + Arduino framework (pioarduino fork, arduino-esp32 3.x / IDF 5.x — matching the reference project).
3. **Web server**: **PsychicHttp** (user's standing preference; wrapper over `esp_http_server`, which the reference project uses raw).
4. **Web assets**: hand-written vanilla HTML/CSS/JS, gzip'd by a pack step into the LittleFS image; no bundler, zero frontend tooling for consumers.
5. **Config store**: split — WiFi credentials in NVS; all other settings as JSON on LittleFS with a settings API consumers extend.
6. **Display stack**: LVGL rejected as heavyweight. A modern, highly-performant alternative is to be selected — see ticket *Display stack selection*.
7. **Captive portal**: WiFi provisioning only, with a network scan list.
8. **Settings surface (MVP)**: timezone, NTP server, brightness, hostname, WiFi re-configure, OTA page, factory reset — nothing more.
9. **mDNS**: yes; hostname configurable, default `smolbase-XXXX` (MAC suffix). AP SSID uses the same derived name.
10. **Timezone model**: curated IANA zone dropdown mapping to POSIX TZ strings (`setenv("TZ")/tzset()`), DST-correct; list shipped as a static JSON asset.
11. **Reprovisioning**: factory reset via settings page; **AP Fallback** is automatic whenever the stored network is unreachable (boot or sustained runtime loss). Timeout/retry policy is an architecture-ticket detail.
12. **Touch**: ship a small driver for the GPIO32 pad exposing tap/long-press events as part of the extension surface.
13. **Stock Screen**: the consumer extension point; ships showing IP, mDNS hostname, and NTP-synced time.

**Hardware facts** (from reference scout of `D:\source\SmolTV-Pro`): ESP32-D0WD-V3 (`esp32dev`), 8 MB flash, **no PSRAM**; ST7789V 240×240 IPS over SPI @60 MHz — SCLK 18, MOSI 23, DC 2, RST 4, CS tied to GND, backlight GPIO 25 (active-LOW LEDC PWM); capacitive touch pad GPIO32 (T9); no USB-serial bridge (internal 6-pad header). Partition table to copy: `D:\source\SmolTV-Pro\partitions.csv` (nvs / otadata / app0+app1 dual OTA 0x220000 each / 3.7 MB littlefs partition labelled `spiffs`).

**Reference project**: `D:\source\SmolTV-Pro` — inspiration only, no code copied. Notable proven patterns: gzip-at-pack-time assets served with `Content-Encoding: gzip`; static send buffers sized under one MSS; heap kept contiguous for mbedTLS; LittleFS mutex around config JSON.
