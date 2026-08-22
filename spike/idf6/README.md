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

**Run 1 — flashed to 10.0.0.32 (fw 0.3.3 → spike), IDF v6.0.2, 2026-08-22.**

| # | Check | Result | Notes |
|---|---|---|---|
| 1 | NVS creds | **PASS** | ssid len 14, pass present — Arduino `Preferences` strings read by raw `nvs_get_str`. Fielded devices need no re-provisioning. |
| 2 | LittleFS `/w` | **PASS** | 6 files in `/w`, 2 dirs at `/`, 56/3776 KB. Non-zero dir count also proves `d_type` is populated. |
| 3 | Panel init | **PASS** | 240x240 |
| 4 | Band push (DMA) | **PASS** | **6180 µs/band against a ~6100 µs theoretical bus floor — 1.3% over.** SPI DMA at 40 MHz is intact. |
| 5 | Touch T9 | **FAIL → diagnosed** | "reads returned 0". Upstream IDF bug, not a driver limitation — see below. Fixed, awaiting run 2. |
| 6 | WiFi join | **PASS** | 10.0.0.32, rssi -54 |
| 7 | TLS api.github.com | **PASS** | http 200, 4096 B, heap floor 147 656 |
| 8 | TLS CDN RSA-4096 | **FALSE PASS** | Reported PASS on `http 404`. The URL was wrong, so it never reached the CDN — see below. **The #119 / ADR 0005 claim remains unverified.** |
| 9 | PsychicHttp native | **PASS** | Serving; these results were read through it |
| 10 | Footprint | **PASS** | free 170 100, largest 110 592, min-ever 139 632, TLS floor 147 656, main task 6400 B stack free of 8192 |
| 11 | sb_fs wrapper | **PASS** | 24 B ArduinoJson round-trip straight through the RAII handle; path ops correct in both directions |
| 12 | Root mount | **PASS** | **`"/w"` resolves unprefixed** — the path problem dissolves |

### Check 8 was a false pass — the important one

The URL used was `/releases/latest/download/smolbase-firmware.bin`, but the real asset is `smolbase-firmware-v0.3.3.bin`. So the 404 came from **github.com**, which never issued a redirect: the handshake that succeeded was github.com's ordinary chain, not the Fastly CDN chain cross-signed by RSA-4096 ISRG Root X1. The pass condition (`status > 0`) accepted it.

**Nothing here yet proves `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` works** — which is the single load-bearing claim behind retiring ADR 0005. Run 2 parses `tag_name` from the API response, builds the real asset URL, and requires `200` **with bytes read**.

### Check 5 is an upstream ESP-IDF bug

`components/esp_driver_touch_sens/hw_ver1/touch_version_specific.c:253`:

```c
ESP_RETURN_ON_FALSE_ISR(type == TOUCH_CHAN_DATA_TYPE_SMOOTH && chan_handle->base->data_filter_fn != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "The software filter has not configured");
```

The condition should be `type != SMOOTH || filter != NULL`. As written it rejects **every** `TOUCH_CHAN_DATA_TYPE_RAW` read with `ESP_ERR_INVALID_STATE`, unconditionally, on ESP32 V1 in 6.0.2.

Reassuringly, `touch_sensor_new_controller`, `new_channel`, `enable` and `start_continuous_scanning` all succeeded — the driver does support this chip, and only the read was wrong. The workaround is to call `touch_sensor_config_filter()` (passing `data_filter_fn = NULL` installs the driver's own default at line 353, satisfying the guard's second clause) and read `TOUCH_CHAN_DATA_TYPE_SMOOTH`. Worth reporting upstream.

### Heap: read this before quoting it

| | Bytes |
|---|---:|
| Arduino `smolbase` env, measured on this device | 133 560 free |
| Spike | 170 100 free (min-ever 139 632) |
| Raw delta | +36 540 |
| Less the static difference (57 600 framebuffer vs 30 720 band scratch) | −26 880 |
| **Like-for-like** | **≈ +9 700** |

So roughly **10 KB**, not the "meaningful recovery" the migration doc implied — and the spike is not running an app, has no settings registry, no `Clock`, no `AssetUpdate`, and never started mDNS, so even that is an order of magnitude rather than a measurement. The heap-headroom argument for migrating is weaker than claimed. The other three arguments are unaffected.
