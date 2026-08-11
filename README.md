# smolbase

[![build](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml/badge.svg)](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml)

Template firmware for the GeekMagic **Small TV Pro** (ESP32, ST7789 240×240).
Clone it, gut `src/app/`, and you start with provisioning, settings, time, OTA,
and a display stack already working — instead of a blank `setup()`.

## Features

- **WiFi provisioning via captive portal** — unconfigured devices open an AP
  (`smolbase-XXXX`); joining it pops a scan-and-join portal. Boot-time AP
  fallback only; runtime drops auto-reconnect silently, forever.
- **Settings web UI over a swappable JSON contract** — the served page is a pure
  static asset rendering `GET /api/settings` (schema + values) and saving a flat
  value map back. Settings your app registers in code appear automatically in an
  "App" section; replace `html/settings.html` for a new look with zero firmware
  changes.
- **NTP + real timezones** — a curated IANA zone list (`html/zones.json`) maps
  to POSIX TZ strings applied with `setenv("TZ")/tzset()`, so DST just works.
- **OTA updates, firmware and filesystem** — upload from the settings page or
  `POST /api/update`; dual app partitions, never open the case after the first
  flash (see [docs/flashing.md](docs/flashing.md)).
- **LittleFS gzip assets** — `html/` is packed to gzip-only files at build time
  (`scripts/pack_fs.py`); PsychicHttp serves them with `Content-Encoding: gzip`.
- **Touch** — the single capacitive pad (GPIO32) is boot-calibrated, debounced,
  and delivered to your active Screen as tap / long-press events.
- **Dual-core layout** — core 0 carries WiFi, the httpd task, and the AP-mode
  DNS pump; core 1 belongs to your app, single-threaded by construction
  ([ADR 0001](docs/adr/0001-consumer-code-single-threaded-core1.md)).
- **mDNS + hostname** — reachable as `smolbase-XXXX.local` (or your configured
  hostname) the moment it joins your network.

## Quick start

1. **Get the code** — use this repo as a GitHub template (or clone it).
2. **Build** — requires [PlatformIO](https://platformio.org/):

   ```
   pio run            # firmware
   pio run -t buildfs # LittleFS image (packs html/ as gzip-only assets)
   ```

   The platform pin (pioarduino, arduino-esp32 3.3.x / IDF 5.5) and exact
   library pins live in `platformio.ini`.
3. **Get firmware on the device** — two paths, pick yours:

   - **Device already has smolbase** (pre-flashed or previously set up): skip
     to step 4 if you still need to provision it onto WiFi, or step 5 if it's
     already on your network. All subsequent firmware and filesystem updates go
     over OTA — you never open the case again.

   - **Blank device**: the board has **no USB-serial bridge**; the first and
     only time you'll use serial is right now. Wire a 3.3 V USB-UART adapter
     to the 6-pad internal header and run:

     ```
     pio run -t upload -t uploadfs --upload-port COM5
     ```

     `COM5` is an example — substitute your adapter's actual port (`/dev/ttyUSB0`
     on Linux/macOS). Full step-by-step: [docs/flashing.md](docs/flashing.md).

4. **Provision** — the screen shows an AP name (`smolbase-XXXX`); join it and
   the captive portal walks you through picking your WiFi network.
5. **Thereafter: OTA** — open `http://smolbase-XXXX.local/settings.html`, set
   timezone and friends, and push future firmware/filesystem builds from the
   Update tab. You never open the case again.
6. **Make it yours** — replace the worked example in `src/app/` with your own
   app: [docs/building-your-app.md](docs/building-your-app.md).

## Hardware

GeekMagic Small TV Pro:

| Component | Detail |
| --- | --- |
| MCU | ESP32-D0WD-V3 (`esp32dev`), 8 MB flash, **no PSRAM** |
| Display | ST7789V 240×240 IPS over SPI @ 40 MHz |
| Display pins | SCLK 18, MOSI 23, DC 2, RST 4, CS tied to GND |
| Backlight | GPIO 25, **active-LOW** LEDC PWM |
| Touch | Single capacitive pad, GPIO 32 (T9) |
| USB | Power only — **no USB-serial bridge**; 6-pad internal serial header |
| Partitions | Dual OTA app slots (2.125 MB each) + 3.7 MB LittleFS (`partitions.csv`) |

Pins and tunables live in one header: `include/smolbase_config.h`.

## Repo tour

```
src/app/        YOUR code — ships with the Boing clock as a worked example
src/core/       plumbing (network, web, config, display, touch, OTA) — read, don't edit
include/        smolbase_config.h: pins, framebuffer mode, timeouts, budgets
html/           web assets (portal, settings UI, zones.json) — packed as gzip to LittleFS
scripts/        pack_fs.py: assembles data/ from html/ at build time
docs/           consumer guides + ADRs
partitions.csv  8 MB layout: nvs / otadata / app0+app1 / littlefs
platformio.ini  pinned platform + libraries
```

## Documentation

- [docs/building-your-app.md](docs/building-your-app.md) — the extension
  surface: App, Screens, routes, settings, events, framebuffers, touch.
- [docs/flashing.md](docs/flashing.md) — first flash over the internal header,
  then OTA forever.
- [docs/adr/](docs/adr/) — architecture decision records.
- [docs/THIRD-PARTY.md](docs/THIRD-PARTY.md) — third-party license notices.
- [CONTEXT.md](CONTEXT.md) — the project vocabulary; terms used in code
  comments and docs are defined there.
- The [wayfinder map](https://github.com/sweetlilmre/smolbase/issues/1) is the
  decision record: every build slice and the reasoning behind it.
