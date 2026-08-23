# smolbase

[![build](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml/badge.svg)](https://github.com/sweetlilmre/smolbase/actions/workflows/build.yml)

Template firmware for the GeekMagic **Small TV Pro** (ESP32, ST7789 240×240).
Clone it, gut `src/app-smolbase/`, and you start with provisioning, settings,
time, OTA, and a display stack already working — instead of a blank `app_main()`.

## Features

- **WiFi provisioning via captive portal** — unconfigured devices open an AP
  (`smolbase-XXXX`); joining it pops a scan-and-join portal. Boot-time AP
  fallback only; runtime drops auto-reconnect silently, forever. The panel shows
  the join in progress from boot — network, and a bar counting down to the AP
  fallback — then hands over to your app.
- **Settings web UI over a swappable JSON contract** — the served page is a pure
  static asset rendering `GET /api/settings` (schema + values) and saving a flat
  value map back. Settings your app registers in code appear automatically in an
  "App" section; replace `html/settings.html` for a new look with zero firmware
  changes.
- **NTP + real timezones** — a curated IANA zone list (`html/zones.json`) maps
  to POSIX TZ strings applied with `setenv("TZ")/tzset()`, so DST just works.
- **OTA updates, firmware and filesystem** — upload from the settings page or
  `POST /api/update`, or let the device **self-update from GitHub releases**
  (check + one-click flash with progress, per-app release assets); dual app
  partitions, never open the case after the first flash (see
  [docs/flashing.md](docs/flashing.md)).
- **LittleFS gzip assets** — `html/` is packed to gzip-only files at build time
  (`scripts/pack_fs.py`); PsychicHttp serves them with `Content-Encoding: gzip`.
- **Native ESP-IDF, no Arduino layer** — one CMake project against the SDK the
  chip actually runs, with a hand-written `sdkconfig.defaults` you can read
  ([ADR 0006](docs/adr/0006-native-esp-idf-framework.md)).
- **Touch** — the single capacitive pad (GPIO32) is boot-calibrated, debounced,
  and delivered to your active Screen as tap / long-press events.
- **Dual-core layout** — core 0 carries WiFi, the httpd task, and the AP-mode
  DNS pump; core 1 belongs to your app, single-threaded by construction
  ([ADR 0001](docs/adr/0001-consumer-code-single-threaded-core1.md)).
- **mDNS + hostname** — reachable as `smolbase-XXXX.local` (or your configured
  hostname) the moment it joins your network.

## Quick start

1. **Get the code** — use this repo as a GitHub template (or clone it).
2. **Build** — requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)
   **v6.0.x** ([ADR 0006](docs/adr/0006-native-esp-idf-framework.md)). Activate
   it, then build an App:

   ```
   idf.py @smolbase.args build   # or @weatherclock.args, @gcm.args
   ```

   That produces both images in `build/smolbase/`: `smolbase.bin` (firmware)
   and `spiffs.bin` (the LittleFS image, with `html/` packed as gzip-only
   assets). In PowerShell quote the argfile: `idf.py '@smolbase.args' build`.

   The IDF pin, the Kconfig settings and the exact component pins live in
   `CMakeLists.txt`, `sdkconfig.defaults` and `main/idf_component.yml`.
3. **Get firmware on the device** — two paths, pick yours:

   - **Device already has stock firmware or a previous smolbase build**: use
     your stock or other firmware OTA mechanism to upload `smolbase.bin`, then
     once smolbase is running upload `spiffs.bin` via
     `http://smolbase-XXXX.local/recover` (or the Update tab in settings if
     the filesystem is already intact). Skip to step 4 if you still need to
     provision onto WiFi, or step 5 if it's already on your network.

   - **Blank device**: the board has **no USB-serial bridge**; the first and
     only time you'll use serial is right now. Wire a 3.3 V USB-UART adapter
     to the 6-pad internal header and run:

     ```
     idf.py @smolbase.args -p COM5 flash
     ```

     That writes the bootloader, partition table, firmware **and** the LittleFS
     image in one go. `COM5` is an example — substitute your adapter's actual
     port (`/dev/ttyUSB0` on Linux/macOS). Full step-by-step:
     [docs/flashing.md](docs/flashing.md).

4. **Provision** — the screen shows an AP name (`smolbase-XXXX`); join it and
   the captive portal walks you through picking your WiFi network.
5. **Thereafter: OTA** — open `http://smolbase-XXXX.local/settings.html`, set
   timezone and friends, and push future firmware/filesystem builds from the
   Update tab. You never open the case again.
6. **Make it yours** — replace the worked example in `src/app-smolbase/` with
   your own app: [docs/building-your-app.md](docs/building-your-app.md).

## Hardware

GeekMagic Small TV Pro:

| Component | Detail |
| --- | --- |
| MCU | ESP32-D0WD-V3, 8 MB flash, **no PSRAM** |
| Display | ST7789V 240×240 IPS over SPI @ 40 MHz |
| Display pins | SCLK 18, MOSI 23, DC 2, RST 4, CS tied to GND |
| Backlight | GPIO 25, **active-LOW** LEDC PWM |
| Touch | Single capacitive pad, GPIO 32 (T9) |
| USB | Power only — **no USB-serial bridge**; 6-pad internal serial header |
| Partitions | Dual OTA app slots (2.125 MB each) + 3.7 MB LittleFS (`partitions.csv`) |

Pins and tunables live in one header: `include/smolbase_config.h`.

## Repo tour

```
src/app-*/      YOUR code — ships with three worked examples (demo clock, weather, CGM)
src/core/       plumbing (network, web, config, display, touch, OTA) — read, don't edit
include/        smolbase_config.h: pins, framebuffer mode, timeouts, budgets
html/           web assets (portal, settings UI, zones.json) — packed as gzip to LittleFS
scripts/        pack_fs.py: assembles data-<app>/ from html/ at build time
                mdns_probe.py: ask a device's mDNS responder directly
docs/           consumer guides + ADRs
partitions.csv  8 MB layout: nvs / otadata / app0+app1 / littlefs
CMakeLists.txt  the build: App selection, compile definitions, fs image
main/           the IDF component manifest (idf_component.yml: pinned deps)
components/     our LovyanGFX wrapper (upstream is cloned in, gitignored)
sdkconfig.defaults  hand-written Kconfig — every line is deliberate
<app>.args      idf.py argfiles: build dir + App selection + that App's sdkconfig
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
