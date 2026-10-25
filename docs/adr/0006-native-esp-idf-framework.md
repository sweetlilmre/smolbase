# 0006 — Build against native ESP-IDF 6, not arduino-esp32

**Status**: accepted (2026-08-23) — supersedes [ADR 0005](0005-hybrid-compile-tls-mpi.md)

## Context

smolbase was built on arduino-esp32 through pioarduino, which was the fastest way to a working device: `LittleFS`, `WiFi`, `Update`, `Preferences`, `String`, and an implicit `setup()`/`loop()` task all arrive for free.

By mid-2026 the cost of that had become concrete rather than theoretical:

- **Config was unreachable.** arduino-esp32 links against *precompiled* IDF static libraries, so mbedTLS buffer sizes and hardware-crypto routing could not be changed from `build_flags`. ADR 0005's answer — pioarduino's hybrid compile — worked, but the price was a ~15-minute IDF rebuild per `platformio.ini` change, a CI cache built solely to dodge it, a PowerShell-only build on Windows, and a generated 3,775-line `sdkconfig.defaults` in the project root that looked like a maintained file and was not.
- **The Arduino layer was a translation cost, not a service.** `String` in 138 places, four duplicated TLS transports, `LittleFS`'s `bool`-returning API over POSIX's `0`-on-success, `Preferences` over `nvs_*`. Every one of those was a shim over an IDF API the firmware could simply call.
- **The Arduino abstractions hid IDF behaviour that mattered.** `Update.h` had no notion of `PENDING_VERIFY`. `configTzTime` quietly held the lwIP core lock, which is the only reason ticket #52 was fixable. `initArduino()` was silently calling `nvs_flash_init()` for us.
- **The platform pin was a fork.** arduino-esp32 3.x on `platformio/espressif32` does not exist; core 3.x is reachable only through the pioarduino community fork. Two layers of indirection stood between this firmware and the SDK it actually runs on.

Phases 1–7b of the migration removed the Arduino coupling incrementally, on a build that still shipped, leaving exactly two libraries in seven places (`LittleFS.h`, `PsychicHttp.h`) plus `main.cpp`.

## Decision

Build as a native ESP-IDF project, pinned to **v6.0.2**.

- `CMakeLists.txt` + `main/CMakeLists.txt` replace `platformio.ini`. Sources stay in `src/`.
- One firmware image per App, chosen at configure time (`-DSMOLBASE_APP=...`), driving `SRC_DIRS` and that App's compile definitions. CI runs a matrix leg per App, so no App can rot — the guarantee a bare `pio run` used to give.
- `sdkconfig.defaults` is **hand-written**. Every line is either a deliberate deviation from the IDF default or an arduino-esp32 value the firmware's behaviour depends on, and each says which.
- PsychicHttp runs in native mode; LittleFS mounts at the **root** through `esp_vfs_littlefs`, so every path has one spelling. LovyanGFX is wrapped as our own component because upstream still uses the pre-v4 `register_component()` style.
- `partitions.csv` is byte-identical. Fielded devices keep their layout, their NVS, and their LittleFS volume.

## Consequences

- **The mbedTLS settings ADR 0005 existed for are now three lines of Kconfig** in a tracked file, applied by an ordinary build. No lib rebuild, no cache keyed on it, no shell restriction. ADR 0005's *thesis* survives verbatim — `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI=y` is still load-bearing, and is set explicitly because its default is conditional on `SOC_RSA_MAX_BIT_LEN`.
- **Measured on the device** (smolbase App, same hardware, same partition layout): free heap 129.5 KB → 166.5 KB, and the 8-bit-accessible pool a TLS handshake actually draws from 77.6 KB → 124.3 KB. Firmware shrank ~1,690,000 → ~1,455,000 bytes. The #119 headroom problem is not merely fixed, it has ~47 KB of margin it never had.
- **Nothing arrives implicitly any more, and that is the point** — but it has to be *written down*, because the failure mode is silence. `app_main()` initialises NVS explicitly; a missing init presents as "the device forgot every setting and credential", not as an error. The consumer loop task's 8 KB stack is declared in `main.cpp` and its low-water mark is reported as `loopStackFree` by `GET /api/status`.
- **IDF 6 ships mbedTLS 4**, where the hash primitives moved to TF-PSA-Crypto and `<mbedtls/sha256.h>` became private. Hashing goes through `psa_hash_*`.
- **The build is warning-clean under IDF's `-Werror` set**, which arduino-esp32 did not apply. That immediately caught an `snprintf` that could truncate a ustar member name into the wrong output path.
- **A first flash still needs the internal serial header**, and now uses `idf.py -p PORT flash` instead of `pio run -t upload`. OTA is unchanged: same endpoints, same partition switch, same rollback guard.
- **The IDF pin is deliberate.** v6.0.2 carries the upstream touch-sensor RAW-read guard bug (espressif/esp-idf#18811), which `Touch.cpp` works around by reading `SMOOTH` through a pass-through filter. That workaround is valid on fixed versions too, so the pin can move — but it should move on purpose.
