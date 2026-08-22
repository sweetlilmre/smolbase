# Phase 0 spike — native ESP-IDF 6 on the Small TV Pro

Throwaway. This directory exists to answer yes/no questions from [docs/research/esp-idf-6-migration.md](../../docs/research/esp-idf-6-migration.md) and then be deleted. Nothing here is production code, nothing here is a template, and no `src/core/` module should ever `#include` from it.

## What it proves

Ten checks, in this order (order matters — see *Getting results out*):

| # | Check | Retires |
|---|---|---|
| 1 | NVS: read `smolbase`/`ssid`+`pass` written by Arduino `Preferences` | Do fielded WiFi credentials survive the framework change? (Phase 5/6) |
| 2 | LittleFS: `esp_vfs_littlefs_register` on the `spiffs` partition, list `/w` | `joltwallet/littlefs` on IDF 6, and that the live asset volume is readable |
| 3 | LovyanGFX: panel init, backlight PWM, fill + text | Open question 3 — the big one |
| 4 | LovyanGFX: 240x64 RGB565 band push at 40 MHz, timed | SPI **DMA** on classic ESP32 under IDF 6 (ADR 0004's hot path) |
| 5 | Touch: `driver/touch_sens.h` on T9/GPIO32, baseline + live reads | Open question 1 — was the top risk |
| 6 | WiFi: `esp_wifi` + `esp_netif` + `esp_event` STA join | The Phase 6 rewrite's foundation |
| 7 | TLS: `esp_http_client` GET `api.github.com/.../releases/latest` | `esp_crt_bundle` on IDF 6 |
| 8 | TLS: follow the release-asset 302 to the CDN and read 8 KB | **The RSA-4096 chain (#119)** with the mbedTLS knobs set in plain `sdkconfig` — the whole ADR 0005 thesis |
| 9 | PsychicHttp in native (non-Arduino) mode: serve the results as JSON | Open question 4 |
| 10 | Footprint: free heap, largest block, min-ever, TLS floor, task high-water | Open question 5 |

## Getting results out

**There is no serial flasher on this bench, so do not count on reading the UART.** The spike reports three ways, in increasing detail:

1. **On the panel** — a PASS/FAIL list, written as each check completes. If check 3 fails you get nothing here, which is itself the answer.
2. **Over HTTP** — once checks 6 and 9 pass, `curl http://<device>/` returns the full results as JSON, including the timings and heap numbers. This is the primary channel.
3. **UART at 115200**, if you happen to have a probe on it.

## Why flashing this is safe

The spike **never calls `esp_ota_mark_app_valid_cancel_rollback()`**. With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (set in `sdkconfig.defaults` here, matching the shipped firmware), the image stays `ESP_OTA_IMG_PENDING_VERIFY` forever, so:

- **Power-cycle the device and the bootloader rolls back to the real firmware.** That is the recovery path, and it needs no serial access.
- Leave it powered and the spike keeps running indefinitely — it never self-confirms.

It uses the same `partitions.csv` as the real build, so it flashes through the running firmware's `/api/update` handler like any other image, into the inactive slot.

**Do not add an OTA-confirm call to this spike.** That is the one edit that turns a safe experiment into a device you cannot recover.

## Build

Requires a real ESP-IDF **6.0.x** install — the PlatformIO tree in `~/.platformio/packages/framework-espidf` is 5.5.5 and cannot build this. See *Install* below.

```powershell
# from spike/idf6, with the IDF environment exported
git clone --depth 1 --branch 1.2.27 https://github.com/lovyan03/LovyanGFX.git components/lovyangfx/upstream
idf.py set-target esp32
idf.py build
```

`components/lovyangfx/CMakeLists.txt` is **ours**, not upstream's. Upstream's root `CMakeLists.txt` still uses the pre-v4 `register_component()` / `COMPONENT_SRCS` style, and whether IDF 6 still carries that compatibility layer is exactly the kind of thing that breaks. Our wrapper does the same file glob through modern `idf_component_register()`, so the spike does not depend on the answer. If upstream's own CMake turns out to work fine on IDF 6, drop the wrapper and simplify.

## Flash and run

```powershell
# build/idf6_spike.bin -> the inactive OTA slot on the running firmware
curl -X POST http://<device-ip>/api/update -F file=@build/idf6_spike.bin
# device reboots into the spike; watch the panel, then:
curl http://<device-ip>/
# when done, power-cycle to roll back to the real firmware
```

## Install (needs your say-so — see the note in the session)

ESP-IDF 6.0 is a multi-GB toolchain install from Espressif's official distribution. It is the vendor SDK for hardware this project already targets, but it is still a software install on a company device, so it is your call and your command to run:

```powershell
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git $HOME\esp\esp-idf
& $HOME\esp\esp-idf\install.ps1 esp32
& $HOME\esp\esp-idf\export.ps1
```

Everything in this directory is written and ready; none of it has been compiled. Treat every file here as unverified until `idf.py build` has run once.

## Results

**Phase 0 complete — 12/12 PASS on hardware (run 5, ESP-IDF v6.0.2, 2026-08-22).**

Full analysis in [docs/research/idf6-phase0-report.md](../../docs/research/idf6-phase0-report.md).

```
PASS  1 nvs creds            ssid len 14, pass present
PASS  2 littlefs /w          6 files /w, 2 dirs /, 56/3776 KB
PASS  3 panel init           240x240
PASS  4 band dma             6185 us/band (bus floor ~6100)
PASS  5 touch t9             smooth baseline 1607 (16/16); RAW=ESP_ERR_INVALID_STATE (upstream, fixed after v6.0.2)
PASS  6 wifi join            10.0.0.32 rssi -58
PASS  7 tls api              http 200, 2048 B, floor 147068
PASS  8 tls cdn rsa4096      v0.3.3 http 206, 8192 B, floor 143236, err=ESP_OK
PASS  9 psychic idf          serving 10.0.0.32/
PASS 10 footprint            free 167908 largest 110592 min 131152
PASS 11 sb_fs wrapper        json 24 B roundtrip, path ops ok
PASS 12 root mount           "/w" resolves unprefixed
heapFree 165924  minEver 131152  tlsFloor 143236  stackFree 6408/8192
```

Check 8 is the one that mattered: a real release asset, the 302 followed to the CDN, and its RSA-4096 cross-signed chain verified with the mbedTLS knobs as plain `sdkconfig` lines. That is ADR 0005's premise, proven.

It took five runs. Runs 1-4 failed on my own errors, documented in the report's process notes — most usefully, check 8's real cause (`buffer_size_tx` defaulting to 512 vs GitHub's ~1.2 KB signed CDN URL) was already commented in `GhUpdate.cpp:163`.

This directory is throwaway. `sb_fs.h` is the only part meant to survive, into `src/core/` at Phase 5.
