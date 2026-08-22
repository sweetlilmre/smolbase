# Migrating smolbase to native ESP-IDF 6 (dropping Arduino)

**Date:** 2026-08-22
**Status:** Assessment / plan. No decision taken. Phase 0 is done: desk verification, a spike that builds on IDF 6.0.2, and one OTA run on the live device. 11 of 12 checks passed; one of those passes was false (see below) and needs a second run.
**Scope:** Replace the `framework = arduino` pioarduino build (arduino-esp32 3.3.11 / ESP-IDF 5.5.5) with a native ESP-IDF 6.x CMake project, on the same hardware (GeekMagic Small TV Pro, ESP32-D0WD, no PSRAM, 8 MB flash) and the same partition layout.

## TL;DR

The migration is **mechanically large but structurally shallow**: ~20 400 lines of firmware, of which roughly 1 200–1 500 lines are Arduino-coupled and need real rewriting. Every third-party dependency already builds natively under ESP-IDF — LovyanGFX ships an IDF component with an explicit `IDF_VERSION_MAJOR >= 6` branch, PsychicHttp 3.x has a first-class native-IDF mode (`#ifdef ARDUINO` throughout, plus its own POSIX filesystem shim and an `ESP_IDF_VERSION_MAJOR >= 6` branch), and ArduinoJson is framework-agnostic. There is no dependency that has to be replaced wholesale.

Estimated effort: **12–18 focused working days**, deliverable as ~8 independently mergeable PRs, of which only the last one changes the framework. Most of the work can be done *while still building against Arduino*, which is what makes this tractable on an OTA-only device.

The single strongest argument for doing it: **ADR 0005 dissolves.** The hybrid-compile hack exists only because pioarduino ships *precompiled* IDF static libraries, so the mbedTLS knobs that keep GitHub OTA alive (`CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` and friends) can only be changed by recompiling those libs locally — a 15-minute rebuild, a fragile CI cache key, and a PowerShell-only build on Windows. Under native IDF every line is compiled from source and those knobs are three ordinary lines in `sdkconfig.defaults`.

**Phase 0 update (2026-08-22):** done, on hardware. Every documentary risk came back clean, the spike builds on IDF 6.0.2, and one OTA run on the live device passed 11 of 12 checks — with SPI DMA measured at 1.3% over the theoretical bus floor. Two things did not survive contact: **the RSA-4096 CDN chain is still unverified** (check 8 passed on a 404 that never reached the CDN), and **the heap argument is roughly 10 KB rather than the "meaningful recovery" claimed below**. Neither blocks the migration; the first needs a second spike run, the second demotes an argument. See [Phase 0 findings](#phase-0-findings-desk-verification).

## Why IDF 6 means dropping Arduino

Not a preference — a constraint. arduino-esp32 3.x is built on ESP-IDF 5.x, and the platform pin in `platformio.ini` is explicit that core 3.x exists *only* through the pioarduino community fork. There is no arduino-esp32 release riding IDF 6, and the Arduino core is not a library you can layer on top of an arbitrary IDF version — it is an IDF component tree tied to a specific IDF minor. So "IDF 6" and "keep Arduino" are mutually exclusive for the foreseeable future, and the two halves of the question are really one question.

## What we gain

| Gain | Detail |
|---|---|
| **ADR 0005 dies** | `custom_sdkconfig` hybrid compile → plain `sdkconfig.defaults`. No ~15 min IDF-lib rebuild on every build-config change, no `idf-hybrid-v1` CI cache key to bust, no "must run `pio` from PowerShell because `idf_tools.py` refuses git-bash" footgun. |
| **Heap headroom** | **Measured, and weaker than this row originally claimed.** The spike's like-for-like gain over the Arduino `smolbase` env is roughly **10 KB** (see `spike/idf6/README.md` for the arithmetic — the raw +36.5 KB delta is mostly a static-buffer difference, not a framework saving). Dropping the Arduino core does remove `String`/`Print`/`Stream`, the Arduino event-loop plumbing, `HardwareSerial` and the `WiFi`/`Network` object graph, but on this evidence the recovery is single-digit-KB, not transformative. Treat heap as a minor argument, not a headline. (The "96 KB free / 37 KB TLS floor" figure quoted earlier is the *weatherclock* env; the device measured here runs `smolbase` at 133 560 bytes free.) |
| **Full Kconfig reach** | mbedTLS ciphersuite/curve trimming, lwIP socket and buffer tuning, `CONFIG_COMPILER_OPTIMIZATION_SIZE`, per-component log levels, task stack sizing. Today most of these are either baked into the precompiled libs or reachable only through the hybrid rebuild. |
| **No single-maintainer fork in the critical path** | The platform pin is a community fork's release ZIP. Native IDF is upstream Espressif. |
| **Better tooling** | IDF emits `compile_commands.json` natively for the real build, which should end the "IDE diagnostics are noise, `pio run` is the arbiter" situation. `idf.py size-components` / `size-files` give real per-component footprint numbers we currently cannot get. |
| **Explicit task model** | ADR 0001's "consumer code is single-threaded on core 1" becomes an `xTaskCreatePinnedToCore` with a stack size we choose, instead of an inherited 8 KB Arduino `loopTask`. |

## What we lose / pay for

- ~~**Template-consumer breakage.**~~ **Mispriced — corrected 2026-08-22.** This repo is a template, but there are no downstream forks yet, so there is no contract to break and nobody to migrate. The build-system change and the `String` → `std::string` change on `ConfigStore`/`Net`/`Secrets` cost nothing outside this repo today. `docs/building-your-app.md` still needs rewriting, but because it documents PlatformIO and `pio run` — not because anyone's code breaks. **This inverts into the strongest timing argument in the document: the migration is free of consumer cost exactly once, and that window is open now.** Every fork taken before the migration converts this line item back into a real cost.
- **The one-click PlatformIO/VSCode flow** is replaced by the ESP-IDF extension + `idf.py`. Different, not worse, but it is a re-learn and a docs rewrite (`docs/flashing.md`, `AGENTS.md`).
- **`pio run` as the single arbiter** goes away; CI, the build-log tee convention, and the multi-env smoke build all need re-expressing in CMake/`idf.py` terms.
- ~~**Unproven combinations.**~~ **Retired.** LovyanGFX-on-IDF-6-on-classic-ESP32 compiles with zero warnings, initialises this ST7789V, and sustains the ADR 0004 band push at 6180 µs against a ~6100 µs bus floor. The touch driver supports the chip (controller, channel, enable and scan-start all succeed) but has an upstream read-path bug — see Phase 0 findings.

## Dependency audit

| Dependency | Native-IDF status | Verdict |
|---|---|---|
| **LovyanGFX 1.2.27** | Ships `CMakeLists.txt` → `boards.cmake/esp-idf.cmake`, a real IDF component. Has an explicit `if(IDF_VERSION_MAJOR GREATER_EQUAL 6)` requirements branch (`nvs_flash efuse esp_lcd driver esp_timer esp_mm esp_driver_ledc esp_driver_i2s hal`). Every legacy-driver include (`driver/i2c.h`, `driver/i2s.h`, `driver/dac.h`, `driver/periph_ctrl.h`) sits behind `__has_include` with a modern-driver branch first, so the legacy paths are dead code on IDF 6. | **Keep, unchanged.** Vendor as a component (git submodule or `idf_component.yml` git dependency). `Display.cpp` and `App.h` need only lose their `<Arduino.h>` include; the whole `SmolPanel` config block is portable as-is. Must still be smoke-tested — the IDF-6 branch existing is not the same as it working on ESP32 classic. |
| **PsychicHttp 3.1.2** | Genuinely dual-target. `PsychicCore.h` gates all Arduino coupling on `#ifdef ARDUINO`; `PsychicFS.h` provides a POSIX `fopen`/`fstat`/`fread` filesystem shim for native builds ("users must have mounted their VFS partition"); there is already an `ESP_IDF_VERSION_MAJOR >= 6` branch (`esp_rom_md5.h` instead of `mbedtls/md5.h`). It is a wrapper over `esp_http_server`, which is IDF-native anyway. | **Keep.** Two caveats: (1) the PlatformIO tarball's `export.include` strips everything but `src/`, so there is no `CMakeLists.txt` in `.pio/libdeps` — vendor from git, or hand-write a 5-line component `CMakeLists.txt`. (2) The native-mode API differs: `String` → `std::string`, and `PsychicUploadCallback`'s filename argument goes `const String&` → `const char*`. That touches `Web.cpp` and `Ota.cpp`. |
| **ArduinoJson 7.4.3** | Framework-agnostic C++17, and PsychicHttp depends on it in native mode too. | **Keep, unchanged.** Consume via the component registry or vendored. |
| **LittleFS** | `joltwallet/littlefs` is already sitting in `managed_components/`. | **Adopt directly** (`esp_vfs_littlefs_register`) — replaces `LittleFS.h`. |
| **mDNS** | `espressif/mdns` is already in `managed_components/`. | **Adopt directly** — replaces `ESPmDNS.h`. |
| **Captive-portal DNS** | No IDF equivalent to Arduino's `DNSServer`. ESP-IDF's `http_server/captive_portal` example ships a small vendorable `dns_server` component. | **Vendor ~150 lines**, or write our own UDP hijack responder; `Portal.cpp` is 27 lines and the surface is trivial. |

## Arduino API inventory

Counts are grep call-sites across `src/` + `include/`, current `main`.

| Arduino API | Sites | IDF replacement | Files affected | Effort |
|---|---:|---|---|---|
| `String` | 138 | `std::string` | Everywhere; public API of `ConfigStore`, `Net`, `Secrets` | **Largest mechanical item.** ~2 days, but see the staging note — doable *before* the framework flip. |
| `WiFi.*` | 28 | `esp_wifi` + `esp_netif` + `esp_event` | `Net.cpp` (272 lines), `Portal.cpp` | **Largest genuine rewrite.** The state machine (boot-join → timeout → AP, runtime reconnect-forever, async scan, AP_STA flip) survives; the plumbing under it triples in size. 2–3 days. |
| `LittleFS` / `FS.h` | 58 | POSIX stdio over `esp_vfs_littlefs` | `AssetUpdate.cpp` (31), `ConfigStore.cpp` (10), `Web.cpp` (8), `Ota.cpp` (5) | Mostly 1:1 (`open`/`read`/`rename`/`remove`); directory iteration becomes `opendir`/`readdir`. 1.5–2 days. |
| `Update.h` | 24 | `esp_ota_ops` (already partly used in `Ota.cpp`/`GhUpdate.cpp`) | `Ota.cpp` | Firmware target is straightforward. The **`U_SPIFFS` target has no `esp_ota` equivalent** — the filesystem upload path becomes explicit `esp_partition_find`/`erase_range`/`write`, and `Update.h`'s "verifies NOTHING for U_SPIFFS" note plus the 0xE9-header guard have to be re-implemented by hand. 1–1.5 days. |
| `HTTPClient` + `NetworkClientSecure` | 22 | `esp_http_client` | `WxHttp.cpp`, `CgmFetch.cpp`, `GhUpdate.cpp`, `AssetUpdate.cpp` — four separate `BundleClient : NetworkClientSecure` subclasses | **The pattern already exists in-repo**: `AssetUpdate`'s tar fetch and `GhUpdate` both drive raw `esp_http_client` / `esp_https_ota` with `crt_bundle_attach`. This is unifying four call sites onto the shape two of them already use, and it kills the `BundleClient` duplication. 1–1.5 days. Net simplification. |
| `Preferences` | 6 | `nvs_flash` / `nvs_open` | `Net.cpp` (WiFi credentials) | `Secrets.cpp` already uses raw NVS and documents why. Copy that. 2 hours. |
| `ESPmDNS` | 6 | `espressif/mdns` component | `Net.cpp` | `mdns_init`/`mdns_hostname_set`/`mdns_service_add`. 2 hours. |
| `Serial.*` | 18 | `ESP_LOGx` (or plain `printf`) | 10 files | Mechanical. Also replaces `-DCORE_DEBUG_LEVEL=0` with `CONFIG_LOG_DEFAULT_LEVEL` — and the ARDUHAL-flooding rationale in `platformio.ini` disappears with ARDUHAL. 3 hours. |
| `millis()` / `micros()` / `delay()` | 39 | `esp_timer_get_time()` / `vTaskDelay` | Everywhere, incl. all five effects | **Do not hand-edit 39 sites.** Add three inline functions to `smolbase_config.h` (or a tiny `compat.h`) and the diff is one file. |
| `touchRead()` | 2 | ESP32 touch-sensor driver | `Touch.cpp` | Small code, **unverified API** — see Open questions. |
| `ESP.getFreeHeap()` / `ESP.restart()` / `ESP.getMaxAllocHeap()` | 6 | `esp_get_free_heap_size()`, `esp_restart()`, `heap_caps_get_largest_free_block()` | `Web.cpp`, `Net.cpp`, `GhUpdate.cpp` | Trivial. |
| `IPAddress` | 3 | `esp_ip4_addr_t` + `esp_ip4addr_ntoa` | `Net.h` | `Net::ip()`'s return type just changes. No consumers to protect. Trivial. |
| `setup()` / `loop()` | 1 | `app_main()` + `xTaskCreatePinnedToCore` on core 1 | `main.cpp` | Small but load-bearing: **pick the stack size deliberately.** Arduino's `loopTask` gave us an implicit 8 KB, and the effects plus the LovyanGFX sprite paths have been running inside that budget. Start at 8 KB and watch the high-water mark. |

Explicitly *not* a problem: no `pinMode`/`digitalWrite`/`analogRead`/`Wire` anywhere, one `random()`, no `map`/`constrain`. The GPIO surface is entirely inside LovyanGFX and the touch pad.

## Build-system migration

This is a separate workstream from the code, and it is not small.

**`platformio.ini` → `CMakeLists.txt` + `sdkconfig.defaults`.** Note that today's `sdkconfig.defaults` is a **3 775-line generated artifact** (header: `# TASMOTA__…`, "Automatically generated file. DO NOT EDIT.") — it is the hybrid-compile's output, not an input we maintain. It gets deleted, and the real config becomes a hand-written `sdkconfig.defaults` of maybe 30 meaningful lines.

**Three apps, no `[env:*]`.** IDF has no env concept. The `build_src_filter` trick (one `makeApp()` per app dir, duplicate/missing symbol as the guard rail) maps cleanly onto a cache variable:

- `main/CMakeLists.txt` takes `-DSMOLBASE_APP=weatherclock`, sets `SRC_DIRS` to `core/` plus `app-${SMOLBASE_APP}/`, and applies that app's `target_compile_definitions` (`SMOLBASE_FRAMEBUFFER`, `SMOLBASE_FW_ASSET_PREFIX`, `SMOLBASE_ASSETS_PREFIX`).
- Per-app config overlays as `sdkconfig.defaults;sdkconfig.<app>` via `SDKCONFIG_DEFAULTS`.
- Per-app build dirs: `idf.py -B build/<app> -DSMOLBASE_APP=<app> build`. An `idf.py @<app>.args` argfile per app keeps the invocation short.
- The "a bare `pio run` builds every env so no app can rot" property becomes a loop in CI plus a convenience script.

**Asset pipeline.** `scripts/pack_fs.py` (a PlatformIO `extra_script` using SCons `Import("env")`) has to stop being a SCons script. Two clean options: (a) keep the packing logic, drop the SCons coupling, and call it from CMake `add_custom_command`/`add_custom_target` with the app name as an argument; or (b) call it from CI and the dev wrapper before `idf.py`, keeping CMake ignorant. Option (a) preserves "the build packs the assets" and is worth the extra CMake. The image itself then comes from `esp_littlefs`'s `littlefs_create_partition_image(spiffs data-<app>/w FLASH_IN_PROJECT)`.

**Keep `partitions.csv` byte-identical.** Deployed devices have this layout, the `spiffs`-subtype partition holding LittleFS, and two 0x220000 OTA slots. Changing any of it strands fielded devices. The label `spiffs` stays even though the volume is LittleFS.

**`sdkconfig` items that must be carried over deliberately:**

- `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI=y` — non-negotiable, GitHub's RSA-4096 cross-signed chain (issue #119).
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` plus `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096`, with IN staying at 16384.
- `CONFIG_HTTPD_WS_SUPPORT=y` if WebSockets are ever wanted (PsychicHttp native builds need it set explicitly).
- Custom partition table CSV; flash size 8 MB; LittleFS via the managed component.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` and the httpd task stack — previously inherited from Arduino defaults.

**CI.** `espressif/esp-idf-ci-action` (or the `espressif/idf` Docker image) replaces `pip install platformio` plus `pio run`. The `idf-hybrid-v1` cache entry — which existed purely to avoid the 15-minute lib rebuild — is deleted; what remains worth caching is `managed_components/` and the build dirs. The release job's per-app `firmware.bin` / `littlefs.bin` / deterministic ustar `assets.tar` steps survive nearly unchanged, only with different paths (`build/<app>/smolbase.bin`). The deterministic-tar guarantee in `docs/research/assets-tar-mechanics.md` is untouched.

## Hard prerequisite: make the cutover OTA-survivable

This device is **OTA-only — there is no serial flasher on the bench.** The good news is that an IDF-6 image is OTA-deliverable: same partition table, same image format, same `esp_ota` write path, so the existing `/api/update` handler on the running Arduino firmware can flash it.

The bad news is a real gap in `Ota::tickRollbackGuard()` (`src/core/Ota.cpp:35`): it confirms the fresh image on **30 s of uptime alone**, with no check that the network ever came up. An IDF-6 image that boots, paints, and then fails to join WiFi — the single most likely first-attempt failure mode, given that `Net.cpp` is the biggest rewrite — would mark itself valid, cancel the rollback, and leave a device that is permanently unreachable and unflashable.

**Fix this before the first IDF-6 OTA:** gate `esp_ota_mark_app_valid_cancel_rollback()` on `Net::isUp()` (or better, on the first successfully served HTTP request), with a timeout that lets the rollback actually fire. This is a small, independently useful change to the *current* Arduino firmware and should ship as its own PR well ahead of the migration.

## Scope decisions (2026-08-22)

Settled, so they stop being relitigated:

**No dual-target build.** One source tree building on both frameworks via `#ifdef ARDUINO` is technically viable at this scale — PsychicHttp and LovyanGFX both do it — and it would let the heap delta be A/B'd on identical source. Rejected anyway: it means carrying dual-target complexity permanently and never being able to delete the Arduino path, which was the point. The per-phase revert path below is sufficient insurance.

**No Arduino compatibility layer beyond leaf primitives.** The reason is that smolbase already has its shims: `Net`, `ConfigStore`, `Ota`, `Portal`, `Display`, `Secrets`. Every large Arduino dependency is already confined to one file — `WiFi.*` (28 sites), `Preferences` (6) and `ESPmDNS` (6) live only in `Net.cpp`; `Update.*` (24) only in `Ota.cpp`; `DNSServer` (4) only in `Portal.cpp`. Emulating those APIs would put a second abstraction underneath the first, wrapping an interface with exactly one caller. Rewriting each module's insides while keeping its namespace is the same work with less surface. What *is* in scope:

- **`compat.h` for leaf calls** — `millis`/`micros`/`delay` (39 sites) and the `ESP.*` helpers (6). Stateless, no lifecycle, and the shim is thinner than what it wraps. That is the test these pass and the object APIs fail.
- **One HTTP helper**, replacing the four duplicated `BundleClient : NetworkClientSecure` subclasses. Framed as deduplication, which the codebase wants regardless of framework — not as compatibility.
- **Possibly a thin RAII file handle** over POSIX for the 58 `LittleFS` sites across four consumers. Judgment call; PsychicHttp solved the same problem for itself in ~40 lines. An owning handle, not an `fs::FS` clone.

Explicitly rejected: emulating `String`, `WiFi`, `Update`, `Preferences`, `HTTPClient`, or `fs::FS`. `String` is the one that would actively hurt — it is the fragmentation-generating type with a silent allocation-failure mode, on the chip where heap is the binding constraint, and shedding it is one of the migration's actual wins.

**No consumer-contract protection.** No downstream forks exist. Signatures change where changing them is cleaner.

Note that none of this weakens the staging below. The phases exist because **this bench has no serial flasher**, so every step must be independently flashable and revertible — a constraint that has nothing to do with API stability.

## Suggested staging

The key insight: **most of the Arduino decoupling can happen while still on Arduino**, because the IDF APIs are all available under arduino-esp32 (it *is* an IDF project). Each phase below lands on `main`, gets OTA-flashed, and gets verified on the real device before the next one starts. Only the last code phase flips the framework, and by then almost nothing depends on Arduino.

| Phase | Work | Framework | Risk retired |
|---|---|---|---|
| **0. Spike** | Throwaway IDF 6 project: LovyanGFX brings up the panel, read the touch pad, `esp_wifi` joins, `esp_http_client` does a TLS fetch of a GitHub release asset with the RSA-4096 chain and the three mbedTLS knobs set in plain `sdkconfig`. Then throw it away. | IDF 6 | **~80% of the total risk**, for 1–2 days. Do not skip this. |
| **1. Rollback guard** | Gate image-confirm on network-up. | Arduino | The one-way-door brick. |
| **2. `compat.h`** | `millis`/`micros`/`delayMs` inlines over `esp_timer`/`vTaskDelay`; `Serial.*` → `ESP_LOGx`; `ESP.*` → `esp_*`. | Arduino | 63 call sites, zero behaviour change. |
| **3. `std::string`** | Module by module, signatures changed freely as we go — with no consumers there is no reason to save the public APIs for last. Arduino `String` and `std::string` coexist happily, so this can be split as finely as review comfort wants. | Arduino | The 138-site mechanical grind, de-risked and reviewable. |
| **4. HTTP unification** | All four `HTTPClient`/`BundleClient` sites onto `esp_http_client`, sharing one helper. | Arduino | TLS behaviour — verified on-device against the real weather/GCM/GitHub endpoints while the old stack is one revert away. Net LOC reduction. |
| **5. Storage** | `LittleFS`/`FS.h` → POSIX plus `esp_vfs_littlefs`; `Preferences` → `nvs`; `Update.h` → `esp_ota_ops` plus explicit partition writes. | Arduino | The scariest I/O paths (asset heal, OTA, settings) proven before the framework moves. |
| **6. Network** | `WiFi`/`ESPmDNS`/`DNSServer` → `esp_wifi` + `esp_netif` + `esp_event` + `mdns` + vendored `dns_server`. | Arduino | The biggest rewrite, still with a working revert path. |
| **7. Build system** | CMake project, per-app selection, asset pipeline, sdkconfig, CI, `idf.py` docs. | **IDF 6** | The flip. `main.cpp` becomes `app_main`; `Web.cpp`/`Ota.cpp` take PsychicHttp's native-mode signatures; `Touch.cpp` takes the new driver; the `<Arduino.h>` includes are deleted. |
| **8. Docs and version** | `AGENTS.md`, `README.md`, `docs/building-your-app.md`, `docs/flashing.md`, a new ADR for the framework decision, ADR 0005 marked superseded, version bump. | IDF 6 | The docs describe a build system that no longer exists — that, not compatibility, is what makes this phase mandatory. |

Phases 1–6 are each individually valuable even if the migration is later abandoned: they remove duplication, unify the HTTP stack, and close a real brick risk.

## Phase 0 findings (desk verification)

Done 2026-08-22 against primary sources: the ESP-IDF v6.0.2 programming guide and v6.0 migration guides, the `release/v6.0` branch Kconfig, the LovyanGFX and PsychicHttp repositories, and the LovyanGFX 1.2.27 tree already sitting in `.pio/libdeps/`.

**ESP-IDF 6.0 is released and stable.** v6.0.2 is what `docs.espressif.com/.../en/stable/` currently serves. This is not a bet on an unreleased SDK.

**The touch driver is fine — the top risk is retired.** IDF 6.0 documents capacitive touch for the original ESP32 explicitly: "V1 [is used by] ESP32", under the unified `driver/touch_sens.h` / `esp_driver_touch_sens` component (introduced in 5.5). V1's "channel value decreases when it is touched" matches `Touch.cpp`'s `< threshold` logic exactly, and absolute-threshold operation — the only mode V1 supports — is precisely what we use. The API is `touch_sensor_new_controller` → `touch_sensor_new_channel` → `touch_sensor_enable` → `touch_sensor_start_continuous_scanning` → `touch_channel_read_data(handle, TOUCH_CHAN_DATA_TYPE_RAW, buf)`. Two ESP32-specific notes: no frequency hopping (one sample config only), and no hardware filter — the driver runs a software filter on `esp_timer`. Neither matters for one pad with a hand-rolled debounce. `Touch.cpp` becomes maybe 20 lines longer.

**All three mbedTLS knobs survive v6.0 under identical names**, and two of them are now *defaults*: `MBEDTLS_ASYMMETRIC_CONTENT_LEN` defaults to `y` and `MBEDTLS_SSL_OUT_CONTENT_LEN` defaults to `4096` (range 512–16384), with `IN` at 16384. `MBEDTLS_LARGE_KEY_SOFTWARE_MPI` still exists, still `depends on MBEDTLS_HARDWARE_MPI`, with the help text "Fallback to software implementation for RSA key lengths larger than SOC_RSA_MAX_BIT_LEN. If this is not active then the ESP will be unable to process keys greater than SOC_RSA_MAX_BIT_LEN" — the exact failure behind #119. Its default is conditional on `SOC_RSA_MAX_BIT_LEN`, so **set it explicitly rather than inheriting the default.** ADR 0005's whole payload is now three lines of `sdkconfig.defaults`.

**LovyanGFX 1.2.27 already contains the IDF 6 work.** Upstream PR #831 ("Updated HAL layer, GPIO handling, SPI, Parallel (I80), HUB75, EPD, PWM/LEDC, and DSI buses for new ESP-IDF v6 APIs") merged 2026-04-05, before 1.2.27. Verified directly in the vendored tree: 16 `ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)` sites, including `platforms/esp32/Bus_SPI.cpp:253` and `platforms/esp32/common.cpp` (LEDC and `i2c_ll_reset_register`) — i.e. the classic-ESP32 SPI and backlight-PWM paths this device actually uses. One caveat found: upstream's own component `CMakeLists.txt` still uses the pre-v4 `register_component()` / `COMPONENT_SRCS` style. The spike sidesteps that with its own wrapper rather than betting on IDF 6 still carrying that compatibility layer.

**PsychicHttp's native-IDF story is real and documented.** Upstream ships both `CMakeLists.txt` and `idf_component.yml` at the repo root (the PlatformIO tarball strips them, which is why `.pio/libdeps` looks Arduino-only), and the README states outright that it "supports both the Arduino framework and native ESP-IDF (no Arduino component required)". Two integration requirements it names: the *project* must declare ArduinoJson in its own `main/idf_component.yml` (it is not inherited), and `sdkconfig` needs `CONFIG_HTTPD_WS_SUPPORT=y` plus `# CONFIG_MBEDTLS_ROM_MD5 is not set`. The README does not claim v6 support, but the source carries an `ESP_IDF_VERSION_MAJOR >= 6` branch (`esp_rom_md5.h` in place of `mbedtls/md5.h`) — which may make that MD5 config note obsolete on 6.x. Worth checking rather than copying blindly.

**The WiFi rewrite lands on a stable API.** The v6.0 WiFi migration guide's removals are all in areas we do not touch: DPP, NAN, FTM, RRM, antenna GPIO, ESP-NOW rate config. The only items in our path are cosmetic — `ESP_IF_WIFI_STA`/`ESP_IF_WIFI_AP` macros gone in favour of `WIFI_IF_STA`/`WIFI_IF_AP`, `WIFI_BW_HT20`/`HT40` → `WIFI_BW20`/`WIFI_BW40`, and `esp_wifi_init` now returning `ESP_ERR_INVALID_STATE` instead of `ESP_OK` when already initialised. New code written against 6.0 simply uses the new names.

**Storage is quieter than feared.** The v6.0 storage changes are concentrated in FATFS and the UART/USB-serial VFS shims. Relevant to us: the legacy `esp_vfs_register`/`esp_vfs_t` API is *deprecated, not removed* (the `esp_vfs_fs_ops_t` form is preferred), and `esp_vfs_console` was renamed `esp_stdio`. Neither touches `esp_vfs_littlefs_register`. `joltwallet/littlefs` 1.22.3 — the version already vendored in `managed_components/` — documents IDF 5.x and 6.0 usage.

**Net effect on the estimate:** unchanged at 12–18 days. Nothing got harder; the touch item shrank from "possibly disqualifying" to "a morning".

### Hardware results (run 1, 2026-08-22)

The spike built clean on IDF 6.0.2 and was flashed OTA to the live device. Full table and caveats in [`spike/idf6/README.md`](../../spike/idf6/README.md). **11 of 12 checks passed, but one of those passes was false and it is the one that matters most.**

Confirmed on hardware:

- **SPI DMA is intact.** The ADR 0004 band push measured **6180 µs against a ~6100 µs theoretical bus floor** — 1.3% over. Open question 3's runtime half is answered.
- **A root mount works.** `esp_vfs_littlefs_register` with `base_path = ""` mounts, and `"/w"` resolves unprefixed. Every existing path constant can stay byte-identical; no prefixing layer, no second path namespace.
- **Arduino-written NVS credentials read back** via raw `nvs_get_str`. Fielded devices need no re-provisioning.
- **PsychicHttp native mode serves requests**, and `sb_fs.h`'s RAII handle round-trips ArduinoJson on-device.
- **LovyanGFX, PsychicHttp and littlefs compile with zero warnings** and link into a 1.03 MB image, 52% of the OTA slot free.

Two corrections:

- **The RSA-4096 CDN chain is still unverified.** Check 8 passed on a `404` that came from github.com, not the CDN — the asset URL was wrong, so no redirect was ever followed and the chain that verified was the ordinary one. `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` therefore remains untested, and it is the load-bearing claim behind retiring ADR 0005. Fixed in the spike; needs run 2.
- **`VSPI_HOST` is removed in IDF 6.** `Display.cpp:22` uses it; `SPI3_HOST` is the same peripheral on classic ESP32, so a rename. Found by the compiler, not by reading.

One upstream defect worth knowing before costing `Touch.cpp`: in 6.0.2, `hw_ver1/touch_version_specific.c:253` guards reads with `type == TOUCH_CHAN_DATA_TYPE_SMOOTH && filter != NULL` where it means `type != SMOOTH || filter != NULL`, so **`RAW` reads are unconditionally rejected** on ESP32 V1. The controller, channel, enable and scan-start calls all succeed — the chip is supported, the read path is buggy. `Touch.cpp` must configure the software filter and read `SMOOTH`. That is still "a morning", but it is a morning plus a workaround comment.

Also worth noting as a small tax across 20 000 lines: IDF builds with `-Werror=missing-field-initializers` and `-Werror=format=`, which the Arduino build does not enforce.

## Open questions — what is still open

Items 1, 2 and most of 4 from the original list are resolved above. What remains needs a compiler and a device, not a browser.

1. ~~ESP32-classic capacitive touch under IDF 6~~ — **resolved.** Supported via `driver/touch_sens.h`; see Phase 0 findings.
2. ~~IDF 6 release status and the mbedTLS `CONFIG_*` names~~ — **resolved.** v6.0.2 is stable; all three knobs survive, two are now defaults.
3. **Does it compile, and does 40 MHz SPI DMA still push bands?** LovyanGFX 1.2.27 has the IDF-6 source changes, but "has the version guards" and "initialises this ST7789V and sustains the ADR 0004 band push" are different claims. The spike times 20 band pushes against the ~6.1 ms bus floor precisely to catch a silent fall back to polled writes. **Open — needs the build.**
4. **PsychicHttp native-mode behavioural gaps.** Integration requirements are now known (see findings), but the *behaviours* `Web.cpp` leans on are still only verified against the Arduino build: the static handler's `name.gz` auto-fallback, `ON_AP_FILTER`/`ON_STA_FILTER`, `rewrite()->setFilter()`, and the `onNotFound` captive catch-all. The filters are the ones to watch — they key off the netif carrying the request, and under native IDF those are `esp_netif` handles we create ourselves rather than ones the Arduino core made. **Open — needs the build, and AP-mode testing on the device.**
5. **Actual heap/flash delta.** Still unmeasured, by design — the spike reports free heap, largest block, min-ever, the TLS floor, and the main-task high-water mark so it can be compared against the Arduino build's 96 KB free / 37 KB TLS floor. **Open — needs the device.**
6. **Windows dev flow.** `idf.py` under PowerShell should be fine and actually *removes* the git-bash prohibition, but the `install.ps1` / `export.ps1` cycle and the VSCode ESP-IDF extension need a walk-through before `docs/flashing.md` can be rewritten honestly. **Open — needs the install.**
7. **New: does the ESP-IDF `dns_server` component still exist in the 6.0 example tree?** The captive-portal DNS hijack was costed at "vendor ~150 lines" on the assumption that example component is still there. Not checked. Low stakes — worst case we write it — but it is an assumption, not a finding.

## Recommendation

Do **Phase 0 and Phase 1** regardless of the wider decision. Phase 1 closes a live brick risk on an OTA-only device and is worth doing this week.

Phase 0's desk half is done and cost no hardware and no install: it retired the potentially-disqualifying touch question, confirmed IDF 6.0 is stable, confirmed the mbedTLS knobs survive verbatim, and confirmed the two libraries we cannot replace have both already done their own IDF-6 work. Nothing found argues against migrating.

Phase 0's remaining half needs an ESP-IDF 6.0 install and one flash of `spike/idf6/`. That is where "compiles" and "40 MHz DMA still works" get settled, and where the heap number that justifies the whole exercise gets measured.

If that comes back clean, the rest is a well-understood grind with a revert path at every step, and the payoff (ADR 0005 gone, real Kconfig control, heap headroom on a chip where heap is the binding constraint, no community-fork dependency) is proportionate to the ~3 weeks.
