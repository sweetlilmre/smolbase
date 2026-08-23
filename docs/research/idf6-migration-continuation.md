# IDF 6 migration — outcome and what is still unverified

**Written:** 2026-08-23. Rewritten from a mid-migration handoff note into a record of the finished thing.
**Companions:** [esp-idf-6-migration.md](esp-idf-6-migration.md) (the plan, as written before any of it happened) · [idf6-phase0-report.md](idf6-phase0-report.md) (Phase 0 evidence) · [littlefs-wrapper-sketch.md](littlefs-wrapper-sketch.md) (the `Fs` design) · [ADR 0006](../adr/0006-native-esp-idf-framework.md) (the decision)

## State

**Done.** Phases 1–9 are complete: `platformio.ini` is gone, the build is a native ESP-IDF v6.0.2 CMake project, all three Apps build warning-clean, and the smolbase App has been flashed and verified on the physical device. `spike/idf6/` has been deleted — its `sb_fs.h` lives on as `src/core/Fs.h` and its scaffolding as the root `CMakeLists.txt`, `main/`, `components/lovyangfx/` and `sdkconfig.defaults`.

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

## Verified on the device

### At the flip (phase 7c)

- Boots, joins, panel up, hostname right, settings persisted from the pre-migration filesystem.
- **Static assets serve from the pre-existing (mklittlefs-built) volume at the new root mount**, gzip fallback included — response sizes match the `.gz` bytes exactly. This was the one-way door: the volume already on the device mounts and reads fine under `esp_vfs_littlefs` at `base_path = ""`.
- **TLS to api.github.com**, i.e. the RSA-4096 chain and therefore `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` coming out of the hand-written `sdkconfig.defaults` — the whole ADR 0005 thesis, now with no lib rebuild behind it. `/api/update/check` also correctly reports `ahead: true` rather than offering a downgrade.
- **SNTP**, synced within 13 s of boot, which is the raw-lwIP-under-the-core-lock `kick()` working (see the note in `Clock.cpp` about why `<esp_sntp.h>` is not included).
- **WiFi scan and its cache**, re-read after the driver consumed the records.
- `loop.stackFree` = 6,276 of 8,192 — the core-1 loop task uses under 2 KB.

### In the phase 9 verification pass

- **mDNS.** Verified by querying the device directly rather than through this host's resolver (`uv run scripts/mdns_probe.py smolbase-2e00 10.0.0.32`, both the QU-unicast and multicast-on-5353 paths): it answers `A smolbase-2e00.local -> 10.0.0.32`, `PTR _http._tcp.local`, and `SRV ...:80`. Service discovery works, not just name resolution. The old note claiming this host has no working `.local` resolver was wrong — `Resolve-DnsName` and `ping` resolve it too.
- **fs-OTA with an image built by `littlefs_create_partition_image`** — after fixing a bug this test existed to find; see below. All six assets on the reflashed volume are byte-identical to the built image, gzip fallback included.
- **`/api/fs` single-file upload**, which is the live exercise of `Fs::mkdirParents` and the new `Fs::replace`.
- **`App::statusJson` with a real App's state**: the weather App reports a completed keyless fetch (`geoCode` 200, `meteoCode` 200, coordinates harvested) with its fields copied under the fetch mux from the httpd task.
- **`onNotFound`'s STA branch**, confirmed by accident while the filesystem was empty: `GET /settings.html` returned the 2,446-byte compiled-in recovery page rather than a 404.
- **`ON_AP_FILTER` staying inert on the STA netif**: `GET /` served `index.html`, not the AP-mode `portal.html` rewrite.
- **Settings survive** two firmware swaps (smolbase → weatherclock → smolbase) and are restorable over `POST /api/settings` after a full fs flash.
- **The rollback guard**, implicitly: a dozen OTA cycles this session, every one confirmed healthy past the 30 s gate with no bootloader fallback.

### The bug the fs-OTA test was for

`littlefs_create_partition_image` was pointed at `data-<app>/w`, but `littlefs-python create` lays the *contents* of its source directory at the image root. The image therefore held `/index.html.gz` while the firmware serves from `/w/` — a valid image that mounts cleanly and 404s everything on it. PlatformIO's `mklittlefs` was handed `data-<env>/` and so got this right by accident, which is why the port did not catch it.

Worth dwelling on how well it hid: the build succeeded, the image carried the `littlefs` magic at offset 8 so `Ota`'s guard passed, the upload answered `{"ok":true,"restarting":true}`, and the device came back joined, healthy, and serving the embedded recovery page for `/settings.html`. Every one of those looks like success. Only comparing *what was served* against what was built showed it. Every release would have shipped an unusable `<app>-littlefs-<tag>.bin`, for all three Apps.

## Still unverified

- ~~**AP mode and the captive DNS hijack.**~~ **Done** — [idf6-ap-mode-verification.md](idf6-ap-mode-verification.md) has the results. The captive path passes 14/14 (DNS hijack, the OS-probe redirects, the loop guard, the portal rewrite, the scan). Getting there took two fixes: a brick-class one-shot `Web::start()`, and the scan's deferred record collection. Serial was attached partway through and is what made the loop fast; attach it before re-running.
- **GitHub self-update end to end.** `/api/update/check` works (TLS to api.github.com, and the semver compare correctly reports `ahead` rather than offering a downgrade). The download-and-flash half needs a release carrying assets built by this toolchain.
- **The once-per-boot STA re-association.** Observed, not explained: 5-15 s after connecting, back with the same IP in ~200 ms, once and then stable. Ruled out: power save (`WIFI_PS_NONE`, pm type 0, zero sleep time, drop unchanged) and every WiFi call of ours. The link is WPA3-SAE with PMF. mDNS now holds through it; nothing else is affected. Low priority, but do not rediscover it from scratch.
- **Panel and touch with human eyes.** The numbers are right — `touch.baseline` 1605 against a 1445 threshold, `app.presentUs` ~26 ms of real panel push — but nobody has looked at the screen or tapped the pad since the flip.
- **CI.** Rewritten and cached, never run. First push will tell. Watch for the LovyanGFX configure-time clone and the `littlefs-python` venv, both of which need network inside the container.

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
- **A .NET SerialPort with default settings DROPS BYTES during the boot burst.** `System.IO.Ports.SerialPort` defaults to a 4096-byte read buffer, and a per-line `ReadLine()` loop in PowerShell cannot keep up with ~11.5 KB/s of boot log. The corruption looks like a firmware bug, because what you get is plausible-but-wrong text:

  ```
  E (1615) psychic: Server startailed - no network interface available.
  E  (114) ychi: erer srtfaled -o netor iterfae available.
  I (1612) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEwithIP: 192.164.1
  ```

  That last line mangles an IP address. Set `ReadBufferSize = 262144` and drain with `ReadExisting()` in a tight loop, doing nothing else in it — no `curl`, no `Test-NetConnection` between reads. Same boot then comes out byte-perfect. Verified both ways.
- **The AP-startup `wifi:no need to send deauth when softap is sending deauth` flood is a CLIENT, not us.** It appears only on boots where something with a saved profile pounces on the AP as it starts beaconing (a `wifi:station: <mac> join` line always follows); with no client in range it does not appear at all. Counts observed: 24, 2, and 0. Driver-internal, benign, and not a conversion regression — `esp_wifi_set_config(WIFI_IF_AP)` runs BEFORE `esp_wifi_start()` here, where arduino-esp32's `APClass::create` configured after starting, which restarts a running AP.
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
- The weather App reported its fetch task's free stack as high-water × 4. ESP-IDF's `uxTaskGetStackHighWaterMark` returns bytes, not the words vanilla FreeRTOS documents — so the figure was four times too generous, in the wrong direction for a number whose job is to warn before an overflow.
- `Platform::largestFreeBlock()` counted `MALLOC_CAP_INTERNAL` while documented as "what a TLS handshake actually needs". The IRAM-leftover region it includes cannot serve a byte buffer, so it answered with memory nobody can spend.
- The weather geocoder persisted `wx_geo_name` and `wx_geo_cc` and never read them back, so `geo.fresh ? geo.cc : ""` blanked the country on every cycle that did not re-geocode — all of them once the cache is warm, including after every reboot. The weather screen draws that country beside the city. Found by reading the status surface immediately after cleaning it up.
