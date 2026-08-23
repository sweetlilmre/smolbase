# IDF 6 migration — outcome and what is still unverified

**Written:** 2026-08-23. Rewritten from a mid-migration handoff note into a record of the finished thing.
**Companions:** [esp-idf-6-migration.md](esp-idf-6-migration.md) (the plan, as written before any of it happened) · [idf6-phase0-report.md](idf6-phase0-report.md) (Phase 0 evidence) · [littlefs-wrapper-sketch.md](littlefs-wrapper-sketch.md) (the `Fs` design) · [ADR 0006](../adr/0006-native-esp-idf-framework.md) (the decision)

## State

**Done.** Phases 1–8 are complete: `platformio.ini` is gone, the build is a native ESP-IDF v6.0.2 CMake project, all three Apps build warning-clean, and the smolbase App has been flashed and verified on the physical device. `spike/idf6/` has been deleted — its `sb_fs.h` lives on as `src/core/Fs.h` and its scaffolding as the root `CMakeLists.txt`, `main/`, `components/lovyangfx/` and `sdkconfig.defaults`.

The device numbers are the reason the exercise was worth doing:

| | Arduino (v0.3.3 lineage) | native IDF 6 | |
|---|---|---|---|
| Free heap (`MALLOC_CAP_INTERNAL`) | 129.5 KB | **166.5 KB** | +37 KB |
| Free 8-bit heap (what TLS draws on) | 77.6 KB | **124.3 KB** | +46.7 KB |
| `firmware.bin` (smolbase App) | ~1,690,000 | **1,455,568** | −234 KB |
| `String` (the type) | 138 | **0** | |
| `LittleFS.` calls | 43 | **0** | |
| Duplicated TLS transports | 4 | **1** | |

Both heap figures come from the same `/api/status` fields computed by the same `Platform::` functions on both sides, so they are comparable. The static-RAM row from the old table is deliberately *not* carried over: PlatformIO's "RAM:" and `idf.py size`'s "DRAM" are not the same ruler, and comparing across a change in how a metric is computed is how a whole afternoon once went missing.

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase` |
| Device | `10.0.0.32`, MAC `b4-bf-e9-c8-2e-00`, hostname `smolbase-2e00`. **IP roams** — find it by MAC via `arp -a` if it moves. |
| ESP-IDF | `~/esp/esp-idf`, tag **v6.0.2**. `& $HOME\esp\esp-idf\export.ps1` to activate. |
| Build | `idf.py '@smolbase.args' build` (quote the `@` in PowerShell — bare `@` is the splatting operator). Also `@weatherclock.args`, `@gcm.args`. ~90 s cold, seconds warm with ccache. |
| Flash | `curl.exe -X POST http://10.0.0.32/api/update -F "file=@build\smolbase\smolbase.bin"`. **No serial flasher and no UART reader on this bench** — every diagnostic comes out over HTTP. |

`idf.py` resolves `@name` to a file called exactly `name`, so the `.args` extension is part of the argument, not a suffix it adds.

## Verified on the device, after the flip

- Boots, joins, panel up, hostname right, settings persisted from the pre-migration filesystem.
- **Static assets serve from the pre-existing (mklittlefs-built) volume at the new root mount**, gzip fallback included — response sizes match the `.gz` bytes exactly. This was the one-way door: the geometry of an image built by `littlefs-python` is not proven, but the *volume already on the device* mounts and reads fine.
- **TLS to api.github.com**, i.e. the RSA-4096 chain and therefore `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` coming out of the hand-written `sdkconfig.defaults` — the whole ADR 0005 thesis, now with no lib rebuild behind it. `/api/update/check` also correctly reports `ahead: true` rather than offering a downgrade.
- **SNTP**, synced within 13 s of boot, which is the raw-lwIP-under-the-core-lock `kick()` working (see the note in `Clock.cpp` about why `<esp_sntp.h>` is not included).
- **WiFi scan and its cache**, re-read after the driver consumed the records.
- `loopStackFree` = 6,276 of 8,192 — the core-1 loop task uses under 2 KB.

## Still unverified — do not claim otherwise

- **mDNS resolution.** This dev host has no working `.local` resolver; it failed before the migration too, so there is no A/B. Needs a host that can resolve mDNS. The `mdns` component is now a registry dependency rather than an IDF built-in, so this is *more* worth checking than it was.
- **The captive-portal DNS hijack, and AP mode generally.** Exercising it means deliberately clearing credentials, and there is no plain restart endpoint (see *Mistakes*). Test it alongside anything else that touches AP entry. PsychicHttp's `ON_AP_FILTER` keys off `esp_netif` handles we now create ourselves, so this is the highest-value untested path.
- **fs-OTA with an image built by `littlefs_create_partition_image`.** Firmware-only OTA is proven; flashing `spiffs.bin` is not. `CONFIG_LITTLEFS_OBJ_NAME_LEN=64` is pinned in `sdkconfig.defaults` because it is written into the superblock of images built this way.
- **GitHub self-update end to end.** The check works; no release yet carries assets built by this toolchain.
- **Touch and the panel with human eyes on them.** The driver calibrates (`touchBaseline` 1606) and the numbers look right, but a tap and a look at the screen is the only real test.
- **CI.** The workflow is rewritten (one `esp-idf-ci-action` matrix leg per App) but has not run. First push will tell. Watch for the LovyanGFX configure-time clone and the `littlefs-python` venv, both of which need network inside the container.

## Hard-won facts — do not re-derive these

- **`ESP.getFreeHeap()` was `MALLOC_CAP_INTERNAL`, not `esp_get_free_heap_size()`** (`MALLOC_CAP_DEFAULT`, 8-bit only). They differ by ~52 KB — the IRAM-leftover region, which cannot serve a byte buffer. `Platform::freeHeap()` uses INTERNAL to stay comparable with every heap number this project has ever recorded, including the #119 "~48 KB free" threshold. `freeHeap8Bit()` is the pool a TLS handshake actually draws from.
- **IDF 6 ships mbedTLS 4.** The hash primitives moved to TF-PSA-Crypto; `<mbedtls/sha256.h>` is now `mbedtls/private/sha256.h`. Use `psa_hash_*` (`psa_crypto_init()` is idempotent). `<mbedtls/base64.h>` survives.
- **`esp_sntp_init()` and `esp_sntp_stop()` are both a bare `tcpip_callback`** — fire-and-forget posts to the tcpip thread. Ticket #52 was a queued stop landing after a fresh init. Arduino's `configTzTime` avoided it by holding the lwIP core lock and calling the raw lwIP functions; `Clock.cpp` reproduces exactly that. **`<esp_sntp.h>` defines deprecated `static inline` shims that shadow `sntp_init` / `sntp_setservername` / `sntp_setoperatingmode` and forward them to the async variants** — including that header would silently reintroduce #52 while the call sites still read as the raw API.
- **`esp_ota_begin` refuses to start while the running image is `PENDING_VERIFY`** (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`). Arduino's `Update.h` had no such constraint. Both `Ota.cpp` and `GhUpdate.cpp` confirm first.
- **`buffer_size_tx` defaults to 512 B but GitHub's signed CDN URL is ~1.2 KB**, so a *redirected* request line does not fit and the request fails with the status stuck at 302. `core/Http` sets 2048.
- **`esp_http_client` de-chunks**; the chunked-body trap (#96) belonged to Arduino's `WiFiClient`. So `core/Http` can and must stream-parse — GitHub's release JSON is tens of KB and will not fit the heap whole.
- **`esp_wifi_scan_get_ap_records` CONSUMES the results.** Arduino's `scanComplete()` let you re-read a count. `Net.cpp` caches on first read, or the portal's polling loop gets one populated response then empty ones.
- **`esp_wifi` has no auto-reconnect.** The old `WiFi.reconnect()` was belt-and-braces; it is now the entire reconnect policy.
- **Touch: `RAW` reads are rejected** by an upstream guard bug (`espressif/esp-idf#18811`, fixed after v6.0.2) in both 5.5.5 and 6.0.2. Read `SMOOTH` with a filter installed. The driver's default filter is an **IIR that adds felt latency** — `Touch.cpp` installs a pass-through instead, because quick taps were being missed. Values **fall** when touched, and the scale is ~1600 where `touchRead()` gave a few hundred, so the margin is a **percentage** (untouched 1607, held ~1270, threshold 10%).
- **Do not zero-initialise IDF config structs blindly.** `touch_channel_config_t{}` sets `charge_speed` to enum 0 (the slowest), not "unset", and the pad then never charges.
- **`PsychicHttp`'s `ON_AP_FILTER`/`ON_STA_FILTER` use pure `esp_netif`** (`ESP_NETIF_DHCP_SERVER`) — but both netifs must exist, so `Net.cpp` creates them up front.
- **In native mode PsychicHttp returns `const char*`** from `uri()`, `host()`, `body()`, `getParam()->value()`, and upload filenames. `==` against a literal compiles fine and compares pointers — every one of those needs `strcmp`. `host()` is backed by a member the next accessor reuses; copy it before touching the request again. `uri()` returns `_uri.c_str()` and is stable.
- **IDF applies `-Werror` to warning classes arduino-esp32 did not**, including `-Wformat` and `-Wformat-truncation`. `uint32_t` is `long unsigned` on this target, so `%u` on a heap figure is a hard error — cast and use `%lu`.
- **`idf_component_register` silently ignores `SRC_DIRS` when `SRCS` is also given**, with one warning in a very long configure log. Use one or the other.
- **`mdns` is no longer bundled with IDF.** It is `espressif/mdns` in the component registry from 6.0 on.

## Mistakes worth not repeating

- **A heredoc turned `'\0'` into a literal NUL byte** in a source file — caught only because `-Werror` flags "null character(s) preserved in literal", and visible beforehand only as `grep` calling the file binary. This is the *second* time a heredoc mangled an escape here (the first double-encoded `"\xC2\xB0"` into `Â°C`, which nothing would have failed on). Non-ASCII or escape-heavy edits go through the editor tools, never a heredoc. Better still: write the code so it needs no escape at all.
- **I called `/api/wifi/forget` while reaching for a reboot** and wiped the device's WiFi credentials; a human had to re-provision by hand. There is no plain restart endpoint on the real firmware. Do not improvise one from the API list.
- **I changed how a metric was computed and then compared across the change**, and spent a long stretch hunting a 52 KB "regression" that did not exist. The ruler, not the memory.
- **I asserted an upstream bug was undiscovered.** One `gh search issues --repo espressif/esp-idf` showed it reported, confirmed and fixed. Search before concluding, especially in an unfamiliar SDK.
- **A pass condition that accepts too little evidence is worse than a failure.** The spike's check 8 originally accepted `status > 0` and reported PASS on a 404 that never reached the CDN — a green tick on the single most important claim in the exercise.

## Bugs this migration surfaced but did not cause

- `jwtPayload` passed `sizeof(buf)` to `mbedtls_base64_decode` while writing a NUL at `buf[outLen]` — one past the end on a full 512-byte decode.
- `findBackupDir` matched backups with `strncmp(path, "/w.", 3)`, silently assuming the volume was mounted at the root. (It is again — but by decision now, not by accident.) A backup that cannot be found never gets restored, which is the whole point of #122.
- `/api/update/check` compared version **strings**, so any mismatch read as "update available" and a device ahead of the latest release was offered a **downgrade**.
- `extractTo` built its output path with an `snprintf` that could truncate a 100-byte ustar member name into a 96-byte buffer, writing the wrong file. Found by `-Werror=format-truncation` on the first native build.
