# IDF 6 migration — continuation note

**Written:** 2026-08-23, at the end of a long session. Read this first if you are picking the migration up cold.
**Branch:** `idf6-migration`, 35 commits ahead of `main`, working tree clean, HEAD `b1aa27c`.
**Companions:** [esp-idf-6-migration.md](esp-idf-6-migration.md) (the plan) · [idf6-phase0-report.md](idf6-phase0-report.md) (Phase 0 evidence) · [littlefs-wrapper-sketch.md](littlefs-wrapper-sketch.md) (the `Fs` design)

## State in one paragraph

Phases 1–6 are done and **verified on the physical device**; 7a and 7b are done and verified. Everything still builds and runs as an **arduino-esp32 build** — the framework flip (7c) has not happened. The device is currently running the branch build (fw `0.4.0-dev`) and is healthy: joined over `esp_wifi`, panel up, touch working (a human has tapped it), settings persisting, TLS to GitHub working. Nothing is half-converted. This is a safe resting point.

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration` |
| Device | `10.0.0.32`, MAC `b4-bf-e9-c8-2e-00`, hostname `smolbase-2e00`. **IP roams** — find it by MAC via `arp -a` if it moves. |
| Running firmware | `0.4.0-dev` (the branch). Latest *release* is `v0.3.3`. |
| PlatformIO | `pio` is NOT on PATH: use `"$HOME\.platformio\penv\Scripts\pio.exe"`. Run from **PowerShell** — the hybrid compile's `idf_tools.py` rejects git-bash/MSYS. |
| ESP-IDF 6 | Installed at `~/esp/esp-idf`, tag **v6.0.2**. `& $HOME\esp\esp-idf\export.ps1` to activate. |
| Spike | `spike/idf6/` — a working IDF 6.0.2 project, 12/12 checks passed on this hardware. **This is the proven scaffolding for 7c.** |

**Build:** `pio run` (all three envs, ~30 s warm). Tee to a log; do not pipe through `Select-Object -First N`, which terminates the pipeline early and silently truncates the log (this cost a bogus "all three green" claim once).

**Flash:** `curl.exe -X POST http://10.0.0.32/api/update -F "file=@.pio\build\smolbase\firmware.bin"`. There is **no serial flasher on this bench** — OTA is the only path, and reading the UART is not possible either, so all diagnostics must come out over HTTP.

**Two build gotchas that wasted time:**
- Concurrent builds **collide**. The hybrid compile writes into the shared `framework-espidf` tree, so a second `pio run` (e.g. in a git worktree) fails with `WinError 32`. Build one at a time.
- A hung `pio`/`cc1plus` **holds source files open**, and `git checkout` then fails with `unable to unlink … Invalid argument` (git reports a lock as EINVAL). If a checkout mysteriously refuses, look for stale compiler processes before blaming clangd. Do **not** switch branches in place to compare builds — use `git worktree`.

## What is done, and what is actually verified

| Phase | Change | Verified |
|---|---|---|
| 1 | Rollback confirm gated on reachability (`isUp() \|\| inApMode()`) | on device |
| 2 | `Platform.h` — 49 leaf calls (millis/delay/heap/restart) | on device |
| 3a–3d | `std::string` sweep: `Secrets`, `ConfigStore`, `Net`, formatters | on device |
| 4a–4c | `core/Http` — four duplicated TLS transports became one | on device (TLS to api.github.com) |
| 5a | `Preferences` → `nvs_*` | on device — **fielded credentials survived** |
| 5b–5c | `core/Fs` — POSIX file I/O | on device — settings persisted across reboot |
| 5d | `Update.h` → `esp_ota_ops` / `esp_partition` | on device — **both** fw and fs targets |
| 6a | `ESPmDNS` → `mdns` component | compile only — see gaps |
| 6b | Own captive-DNS responder (replaces `DNSServer`) | compile only — see gaps |
| 6c | WiFi → `esp_wifi`/`esp_netif`/`esp_event` | on device — joined, scan+cache correct |
| 7a | `Touch` → `driver/touch_sens.h`, percentage margin | on device, **including a human tapping it** |
| 7b | `Serial` → `printf`, `VSPI_HOST` → `SPI3_HOST`, `<Arduino.h>` gone | on device |

**Genuinely untested, do not claim otherwise:**
- **mDNS resolution.** This dev host has no working `.local` resolver — it failed before the migration too, so there is no A/B. Needs a host that can resolve mDNS.
- **The captive-portal DNS hijack.** Exercising it needs the device in AP mode with a client attached, i.e. deliberately clearing credentials. Test alongside anything else that touches AP entry.
- **AP mode generally**, beyond one accidental round trip (see *Mistakes* below) which did confirm `nvs_erase_all` → AP → portal → re-provision works.

## Metrics, for comparison after 7c

| | main | branch now |
|---|---|---|
| `String` (the type) | 138 | **26** |
| `LittleFS.` calls | 43 | **6** |
| Duplicated TLS transports | 4 | **1** |
| Static RAM (smolbase env) | 115,768 | 115,784 |
| Free heap on device | ~135.3 KB | ~132–134 KB |
| `firmware.bin` | 1,731,696 | ~1,690,000 |

Heap is at parity. The ~2.6 KB gap after 6c is most plausibly the AP netif now created up front (see 6c note) — small, explainable, deliberately not chased.

## Phase 7c — the remaining work

This is the only step with **no incremental fallback**: one commit where the build system, the HTTP library mode, the filesystem mount and the entry point all change together. Give it a clear run.

Remaining Arduino coupling is exactly **two libraries in seven places**:

```
<LittleFS.h>     ConfigStore.cpp (the mount), Ota.cpp (LittleFS.end), Web.cpp (serveStatic)
<PsychicHttp.h>  Web.cpp, Ota.cpp, GhUpdate.cpp, WeatherApp.cpp   (Arduino-mode String)
```

Plus `main.cpp`'s `setup()`/`loop()`/`Serial.begin(115200)`, and the 26 `String`s that live inside those PsychicHttp handlers.

**The work:**

1. **CMake project.** Root `CMakeLists.txt` + `main/CMakeLists.txt`. Base it on `spike/idf6/` — that scaffolding is proven on this hardware. Sources live at `../src`, so `main/CMakeLists.txt` registers `SRCS` with relative paths out of `main/`.
2. **Per-app selection**, replacing the three `[env:*]`. A cache variable (`-DSMOLBASE_APP=weatherclock`) driving `SRC_DIRS` (`core/` + one `app-*/`) and that app's `target_compile_definitions` (`SMOLBASE_FRAMEBUFFER`, `SMOLBASE_FW_ASSET_PREFIX`, `SMOLBASE_ASSETS_PREFIX`). Note the **name mapping**: env `smolbase`→`app-smolbase`, `weatherclock`→`app-weather`, `gcm`→`app-gcm`. Per-app build dirs (`idf.py -B build/<app>`) and an `@<app>.args` argfile each. CI loops the three so no app can rot.
3. **`sdkconfig.defaults`, hand-written.** The current 3,775-line one is a *generated artefact* of the hybrid compile — delete it. `spike/idf6/sdkconfig.defaults` is the ~30-line real thing, including the three mbedTLS lines that ADR 0005 exists for. **Set `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI=y` explicitly** — its default is conditional on `SOC_RSA_MAX_BIT_LEN` and must not be inherited.
4. **PsychicHttp → native mode.** Vendor from git (the PlatformIO tarball strips `CMakeLists.txt`/`idf_component.yml`); upstream tags carry **no `v` prefix** (`3.1.2`, not `v3.1.2`). The project must declare **ArduinoJson itself** in `main/idf_component.yml` — it is not inherited. Needs `CONFIG_HTTPD_WS_SUPPORT=y`. Handler signatures change: `String` → `std::string`, and `PsychicUploadCallback`'s filename goes `const String&` → `const char*`.
5. **LittleFS → `esp_vfs_littlefs`, mounted at the ROOT.** Spike check 12 proved `base_path = ""` works and `/w` resolves unprefixed. Then `SMOLBASE_FS_MOUNT` becomes `""` and the two path spellings in `smolbase_config.h` collapse to one. `serveStatic` moves to PsychicHttp's native `psychic::FS` (POSIX), so `Web.cpp`'s dual spelling goes away.
6. **`main.cpp` → `app_main`** plus an `xTaskCreatePinnedToCore` on **core 1** (ADR 0001). Arduino gave an implicit 8 KB `loopTask`; size it deliberately and watch the high-water mark. Drop `Serial.begin`.
7. **`nvs_flash_init()` explicitly.** Neither `Net.cpp` nor `Secrets.cpp` calls it — `initArduino()` has been doing it. A missing init presents as "all settings and credentials vanished". This is written down in `Net.cpp` too.
8. **LovyanGFX as a component.** Use `spike/idf6/components/lovyangfx/CMakeLists.txt` — **ours**, not upstream's, because upstream still uses the pre-v4 `register_component()` style. Clone tag `1.2.27` (no `v` prefix).
9. **Asset pipeline off SCons.** `scripts/pack_fs.py` uses `Import("env")`. Either keep the packing logic and call it from a CMake `add_custom_command`, or call it from CI/a wrapper before `idf.py`. The image then comes from `littlefs_create_partition_image(spiffs data-<app>/w FLASH_IN_PROJECT)`.
10. **Keep `partitions.csv` byte-identical.** Fielded devices have this layout; the `spiffs`-subtype partition holds LittleFS and the label stays.
11. **CI.** `espressif/esp-idf-ci-action` (or the `espressif/idf` image) replaces `pip install platformio` + `pio run`. Delete the `idf-hybrid-v1` cache entry — it only existed to dodge the 15-minute lib rebuild. Release job paths become `build/<app>/smolbase.bin`; the deterministic ustar step is unchanged.
12. **`.gitignore`:** remove the `/CMakeLists.txt` line — Phase 7 makes it a real tracked file.
13. **Pin the IDF deliberately.** `release/v6.0` HEAD or v6.0.3+ has the touch RAW fix; on v6.0.2 the pass-through workaround already in `Touch.cpp` is what makes reads work, and it is valid on fixed versions too.

**Then Phase 8:** rewrite `AGENTS.md`, `README.md`, `docs/building-your-app.md`, `docs/flashing.md`; new ADR for the framework decision; mark **ADR 0005 superseded**; delete `spike/idf6/` once absorbed (`sb_fs.h` already lives on as `src/core/Fs.h`).

## Hard-won facts — do not re-derive these

- **`ESP.getFreeHeap()` is `MALLOC_CAP_INTERNAL`, not `esp_get_free_heap_size()`** (which is `MALLOC_CAP_DEFAULT`, 8-bit only). They differ by ~52 KB — the IRAM-leftover region, which cannot serve a byte buffer. `Platform::freeHeap()` uses INTERNAL to stay comparable with every heap number this project has ever recorded, including the #119 "~48 KB free" threshold. `freeHeap8Bit()` is the pool a TLS handshake actually draws from and is reported as `heapFree8Bit`.
- **`esp_ota_begin` refuses to start while the running image is `PENDING_VERIFY`** (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`). Arduino's `Update.h` had no such constraint. Both `Ota.cpp` and `GhUpdate.cpp` now confirm first.
- **`buffer_size_tx` defaults to 512 B but GitHub's signed CDN URL is ~1.2 KB**, so a *redirected* request line does not fit and the request fails with the status stuck at 302. `core/Http` sets 2048. This cost three spike runs to rediscover while the fix sat commented in `GhUpdate.cpp`.
- **`esp_http_client` de-chunks**; the chunked-body trap (#96) belongs to Arduino's `WiFiClient`. So `core/Http` can and must stream-parse — GitHub's release JSON is tens of KB and will not fit the heap whole.
- **`esp_wifi_scan_get_ap_records` CONSUMES the results.** Arduino's `scanComplete()` let you re-read a count. `Net.cpp` caches on first read, or the portal's polling loop would get one populated response then empty ones.
- **`esp_wifi` has no auto-reconnect.** The old `WiFi.reconnect()` was belt-and-braces; it is now the entire reconnect policy.
- **Touch: `RAW` reads are rejected** by an upstream guard bug (`espressif/esp-idf#18811`, fixed after v6.0.2) in **both** 5.5.5 and 6.0.2. Read `SMOOTH` with a filter installed. The driver's default filter is an **IIR that adds felt latency** — `Touch.cpp` installs a pass-through instead, because a user reported quick taps being missed. Values **fall** when touched, and the scale is ~1600 where Arduino's `touchRead()` gave a few hundred, so the margin is a **percentage** (measured: untouched 1607, held ~1270, threshold 10%).
- **Do not zero-initialise IDF config structs.** `touch_channel_config_t{}` sets `charge_speed` to enum 0 (the slowest), not "unset", and the pad then never charges.
- **PsychicHttp's `ON_AP_FILTER`/`ON_STA_FILTER` use pure `esp_netif`** (`ESP_NETIF_DHCP_SERVER`), no Arduino dependency — but both netifs must exist, so `Net.cpp` creates them up front.

## Mistakes worth not repeating

- **I called `/api/wifi/forget` while reaching for a reboot** and wiped the device's WiFi credentials; the user had to re-provision by hand. There is no plain restart endpoint on the real firmware — the spike has `/restart`, the firmware does not. Do not improvise one from the API list.
- **I changed how a metric was computed and then compared across the change**, and spent a long stretch hunting a 52 KB "regression" that did not exist. Ruled out uptime, TLS, static RAM and iostream init before instrumenting boot on both branches — which is what finally showed the gap was already present before `setup()`. The ruler, not the memory.
- **I asserted an upstream bug was undiscovered.** One `gh search issues --repo espressif/esp-idf` showed it reported, confirmed and fixed. Search before concluding, especially in an unfamiliar SDK.
- **A shell heredoc mangled `"\xC2\xB0"`** into double-encoded UTF-8; the panel would have rendered `Â°C` with nothing failing. Non-ASCII or escape-heavy edits go through the editor tools, never a heredoc.
- **A pass condition that accepts too little evidence is worse than a failure.** The spike's check 8 originally accepted `status > 0` and reported PASS on a 404 that never reached the CDN — a green tick on the single most important claim in the exercise.

## Bugs this migration surfaced but did not cause

Worth mentioning if anyone asks what the port bought beyond the framework:

- `jwtPayload` passed `sizeof(buf)` to `mbedtls_base64_decode` while writing a NUL at `buf[outLen]` — one past the end on a full 512-byte decode.
- `findBackupDir` matched backups with `strncmp(path, "/w.", 3)`, silently assuming the volume was mounted at the root. A backup that cannot be found never gets restored, which is the whole point of #122.
- `/api/update/check` compared version **strings**, so any mismatch read as "update available" — a device ahead of the latest release was offered a **downgrade**, and `GhUpdate`'s `sameVer` gate (also a string compare) would not have stopped it.
