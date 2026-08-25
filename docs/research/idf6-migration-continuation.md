# IDF 6 migration — continuation note

**Written:** 2026-08-23, at the end of the session that finished the port. Read this first if you are picking smolbase up with no context.
**Branch:** `idf6-migration`, **55 commits ahead of `main`** — this note is the tip. Working tree clean; the port's last code commit is `3bf86f6`.
**TLS performance work has its own record and handover:** [mbedtls4-perf-spike.md](mbedtls4-perf-spike.md).
**Companions:** [esp-idf-6-migration.md](esp-idf-6-migration.md) (the plan, written before any of it happened) · [idf6-phase0-report.md](idf6-phase0-report.md) (Phase 0 evidence) · [idf6-ap-mode-verification.md](idf6-ap-mode-verification.md) (AP-mode procedure and results) · [idf-tuning-levers.md](idf-tuning-levers.md) (Kconfig measurements) · [littlefs-wrapper-sketch.md](littlefs-wrapper-sketch.md) (the `Fs` design) · [ADR 0006](../adr/0006-native-esp-idf-framework.md) (the framework decision)

## State in one paragraph

**The port is done and verified on hardware.** `platformio.ini` is gone; this is a native ESP-IDF v6.0.2 CMake project. All three Apps build warning-clean. The device has been flashed, provisioned through the captive portal, AP-mode tested, self-updated from GitHub end to end, and is running the branch build now. Phases 1–9 are complete and `spike/idf6/` has been absorbed and deleted. What remains is **CI** (rewritten, never executed) plus two small documentation items. Nothing is half-converted; this is a safe resting point.

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration` |
| ESP-IDF | `~/esp/esp-idf`, tag **v6.0.2**. Activate with `& $HOME\esp\esp-idf\export.ps1`. |
| Device | `10.0.0.32`, MAC `b4-bf-e9-c8-2e-00`, hostname `smolbase-2e00`. **IP roams** — find it by MAC via `arp -a`, or `uv run scripts/mdns_probe.py smolbase-2e00`. |
| Serial | **A UART adapter is on COM5**, with RTS auto-reset wiring, so no manual GPIO0 boot-mode dance. This is new and it changes how the project can be worked on — see *Serial is attached* below. |
| Build | `idf.py '@smolbase.args' build`. **Quote the `@` in PowerShell** — bare `@` is the splatting operator. Also `@weatherclock.args`, `@gcm.args`. `idf.py` resolves `@name` to a file called exactly `name`, so `.args` is part of the argument. |
| Flash | OTA (preferred): `curl.exe -X POST http://10.0.0.32/api/update -F "file=@build\smolbase\smolbase.bin"`. Serial: `idf.py '@smolbase.args' -p COM5 -b 460800 app-flash`. |

Artifact sizes now: smolbase 1,474,080 · weatherclock 1,240,560 · gcm 1,219,584. Filesystem image 3,866,624 for all three.

**After any OTA, wait out the 30 s rollback guard before doing anything else that reboots.** Flashing the filesystem 15 s after an app OTA reverted the device to the previous image — the guard working exactly as designed, and easy to trip by accident.

### Build times, and why a build is sometimes five minutes

A warm no-op build is **~4 s**; a source change is seconds. But **any change to `sdkconfig.defaults` costs a full ~280 s rebuild**, because `sdkconfig.defaults` only supplies values *absent* from the generated `sdkconfig` — an existing `sdkconfig` wins. To apply a defaults change you must delete `build/<app>/sdkconfig` and let it regenerate, which changes compile flags for every IDF component. That is the mechanism, not gratuitous slowness. Build only the App you are iterating on; build all three before committing.

Tee build output to `build\smolbase.log` whichever App you are building — a human may be tailing that exact path.

## Where things live

```
CMakeLists.txt         the build: App table (-DSMOLBASE_APP=...), compile defs, fs image
main/CMakeLists.txt    the single component; SRC_DIRS only (adding SRCS silently disables it)
main/idf_component.yml pinned deps: espressif/mdns, joltwallet/littlefs, psychichttp (git), arduinojson
components/lovyangfx/  our wrapper; clones the pinned tag at configure time
sdkconfig.defaults     hand-written, every line justified in a comment
<app>.args             idf.py argfiles: -B build/<app>, -D SMOLBASE_APP, -D SDKCONFIG
scripts/pack_fs.py     assembles data-<app>/w from html/ + the App's html overlay
scripts/mdns_probe.py  asks a device's mDNS responder directly, bypassing the host resolver
```

## Verified on hardware

All observed on the device, not inferred.

- Boots, joins, panel up, **touch confirmed by a human tapping it**, settings persisting.
- **Static assets** from LittleFS mounted at the root through PsychicHttp's native `psychic::FS`, gzip fallback included — every asset byte-identical to the built image.
- **TLS to api.github.com**; `/api/update/check` correctly reports `ahead` rather than offering a downgrade.
- **SNTP**, synced within 13 s of boot — the raw-lwIP-under-the-core-lock `kick()` working.
- **mDNS**: A record, `PTR _http._tcp`, `SRV :80`, up 8 ms after the IP lands.
- **WiFi scan** and its result handoff, including the portal's polling loop.
- **AP mode and the captive portal: 14/14.** DNS hijack on four probe hostnames, `302` for all three OS connectivity probes, the self-addressed loop guard, the portal rewrite, the scan. See [idf6-ap-mode-verification.md](idf6-ap-mode-verification.md).
- **Provisioning end to end** — credentials cleared, AP joined from a phone, network picked, device rejoined.
- **fs-OTA** with an image built by `littlefs_create_partition_image`.
- **Serial flash** over the internal header on the IDF build.
- **GitHub self-update end to end** — the only way to reach four things that had never executed: AssetUpdate's ustar walker, its backup/rename/heal, GhUpdate's `esp_https_ota`, and the mbedTLS-4 PSA hashing that replaced `mbedtls_sha256_*`. Completes in 41.8 s, zero watchdog triggers, six assets extracted and digest-verified.
- **The rollback guard**, twice: passively across dozens of OTA cycles, and actively when a too-early filesystem flash correctly reverted the device.

### Measured outcomes

| | Arduino (v0.3.3) | native IDF 6 | |
|---|---|---|---|
| Free heap (`MALLOC_CAP_INTERNAL`) | 129.5 KB | **166.5 KB** | +37 KB |
| Free 8-bit heap (what TLS draws on) | 77.6 KB | **124.3 KB** | +46.7 KB |
| TLS handshake to GitHub | 4.12 s | **4.21 s** | parity |
| Self-update, end to end | — | **41.8 s**, 0 watchdog trips | |
| `String` (the type) | 138 | **0** | |
| `LittleFS.` calls | 43 | **0** | |
| Duplicated TLS transports | 4 | **1** | |

Both heap figures come from the same `/api/status` fields computed by the same `Platform::` functions on both sides. The static-RAM row from earlier drafts is deliberately gone: PlatformIO's "RAM:" and `idf.py size`'s "DRAM" are not the same ruler, and comparing across a change in how a metric is computed is how an afternoon once went missing.

## Still open

1. **CI has never run.** `.github/workflows/build.yml` is rewritten — one `esp-idf-ci-action` matrix leg per App, with caches for managed components, the LovyanGFX clone, and per-App ccache — but has not executed once. Watch the LovyanGFX configure-time clone and the `littlefs-python` venv; both need network inside the container. Also confirm the `actions/*@v7` pins resolve: that is this repo's existing convention, not a normal one.
2. **`CONFIG_ESP_TASK_WDT_PANIC` asymmetry is undocumented.** arduino-esp32 shipped it `y`; we are on IDF's default `n`. Since the TLS fix the watchdog no longer trips, so this is far less dangerous than it was, but anyone "restoring parity" with the old build would reboot the device mid-OTA if a handshake ever outran the timeout. One comment in `sdkconfig.defaults` closes it.
3. **Ready to report upstream, with a root cause and a patch.** mbedTLS 4.1.0 is 15–41% slower than 3.6.6 per bignum primitive at identical call counts — not the "roughly 2×" this note used to claim (that was the contaminated end-to-end figure, ~2.07 s of which was our own unbuffered reader). **The cause is found:** three lines in `mbedtls_mpi_core_sub()` and `mbedtls_mpi_core_mla()`, changed in 2024 to be constant-time via `mbedtls_ct_uint_lt()`, which has hand-written assembly only for Arm/AArch64/x86/x86-64 and on every other architecture falls back to generic C built on six `asm volatile` barriers that GCC then declines to inline at `-Os`. Proven by patch-and-measure: reverting the three lines recovers 100% of the gap, and a constant-time-preserving fix recovers 111%. Full write-up, disassembly and patch: **[mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md)**. Affects RISC-V, MIPS and PowerPC too, by the same preprocessor logic. Still in `main`; absent from the 3.6 LTS; no matching report found — but that was a keyword search, so do not assert it is undiscovered.
4. **The CGM app has never been flashed**, so `Secrets` set/get, `jwtPayload`'s base64 and `CgmFetch`'s PSA hashing have never run. The weather App runs, but the device has no OWM key, so only the keyless open-meteo path has executed.
5. **The once-per-boot STA re-association.** Observed, not explained: 5–15 s after connecting, back with the same IP in ~200 ms, once then stable. Ruled out: power save (`WIFI_PS_NONE`, pm type 0, zero sleep time, drop unchanged) and every WiFi call of ours. The link is WPA3-SAE with PMF. mDNS now rides through it. Low priority, but do not rediscover it from scratch.

## Serial is attached — use it

This is the biggest change to how the project can be worked on. Previously the bench was OTA-only with no UART, so every diagnostic had to come out over HTTP and a brick meant opening the case.

- **Reset without reflashing:** pulse RTS (`$sp.RtsEnable = $true`, 150 ms, `$false`) with DTR low.
- **Read the boot log**, which is where `psychic: Server start failed` and the WiFi driver's state machine live. Neither is visible over HTTP.
- **Decode a backtrace:** `xtensa-esp32-elf-addr2line -pfiaC -e build/smolbase/smolbase.elf <addrs>`. This located the TLS cost.
- **Poor man's profiler:** lower `CONFIG_ESP_TASK_WDT_TIMEOUT_S` to 1 temporarily and the watchdog samples the stack repeatedly through a slow operation.
- **Force a code path without disturbing state:** a temporary `if (true)` in `Net::begin()` gave AP mode without clearing the user's credentials.

**But read the serial correctly.** A default `System.IO.Ports.SerialPort` drops bytes during the boot burst and produces plausible-but-wrong text — including a mangled IP address, which reads exactly like a firmware bug:

```
E (1615) psychic: Server startailed - no network interface available.
E  (114) ychi: erer srtfaled -o netor iterfae available.
I (1612) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEwithIP: 192.164.1
```

Set `ReadBufferSize = 262144` or larger and drain with `ReadExisting()` in a tight loop, doing nothing else inside it — no `curl`, no `Test-NetConnection` between reads. The same boot then comes out byte-perfect. Verified both ways.

## Hard-won facts — do not re-derive these

### TLS performance (this session's largest piece of work)

- **The negotiated suite is `TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256` over an all-ECDSA chain** (`*.github.com` EC-256, Sectigo EC-256/384). **There is no RSA operation in a GitHub handshake at all.**
- **The ESP32 has no ECC accelerator.** `MBEDTLS_HARDWARE_ECC` and `MBEDTLS_HARDWARE_ECDSA_VERIFY` both `depend on` SOC caps this chip lacks. Elliptic-curve maths is software on every IDF version. Do not go hunting for a hardware ECC path.
- **`CONFIG_MBEDTLS_HARDWARE_MPI` is deliberately OFF, and that is a speed-up.** All four permutations, measured with the buffered reader in place:

  | `HARDWARE_MPI` | `ECP_FIXED_POINT_OPTIM` | handshake | watchdog |
  |---|---|---|---|
  | y | n | 6.85 s | trips every request |
  | y | y | 6.50 s | trips every request |
  | n | n | 5.92 s | trips every request |
  | **n** | **y** | **4.21 s** | **clean** |

  The peripheral is an *RSA* accelerator. Its only effect on an ECDSA handshake is intercepting `mbedtls_mpi_mul_mpi`, and the port has **no lower size threshold**: every 256-bit multiply pays a crypto-lock acquire, a clock enable, two hardware passes (the ESP32 needs two), a clock disable and a lock release. **That per-call overhead is now measured**: at 256 bits the peripheral costs 19.5 µs against software's 9.8 µs, and the crossover is between 256 and 512 bits — from 512 up it wins, and at 2048/4096 bits it is 3.7–4.0× faster.
- **This is not a mbedTLS 4 behaviour, and the sentence that used to be here saying it was is wrong.** The claim was that v4's ECP calls `mul_mpi` far more often than 3.x did. It was never counted; it is now, and both versions make **exactly 5817 calls per P-256 verify** (3912 with fixed-point ECP). mbedTLS 3.6.6 is **17.8% faster with the accelerator off** as well. The peripheral is wrong for ECC-sized operands on this chip in every version — it explains none of the version gap. See [mbedtls4-perf-spike.md](mbedtls4-perf-spike.md).
- **The two levers do NOT interact, in the crypto.** Offline they are independent multiplicative wins (predicted 0.62 vs measured 0.63 of baseline on v4). The end-to-end matrix appeared to show a strong interaction — fixed-point ECP worth 28% with the accelerator off but 5% with it on — but three of those four cells tripped the watchdog on every request and the fourth did not, which is the more likely explanation. Measure them together anyway; just do not build a mechanism on the interaction.
- **#119 is still safe, by a simpler route.** The failure was the RSA accelerator being unable to complete the 4096-bit verify in GitHub's ISRG Root X1 cross-signed CDN chain. With no hardware MPI there is no hardware limit to exceed — every RSA op is software, so it just works. `LARGE_KEY_SOFTWARE_MPI` existed only to add that fallback, depends on `HARDWARE_MPI`, and is gone with it deliberately. **Validated by a full self-update.**
- **`MBEDTLS_COMPILER_OPTIMIZATION_PERF` makes TLS SLOWER** (9.38 s vs 8.49 s): code growth costs more in flash-cache misses than `-O2` wins.
- **Already ruled out, do not re-check.** The Arduino build's own IDF config is on disk at `~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig`, and every relevant option is identical to ours — MPI, `MPI_USE_INTERRUPT`, `ECP_NIST_OPTIM`, the curve list including X25519, TLS 1.2-only, CPU 240 MHz, `-Os`, no PSRAM, DIO/40M. The ESP bignum port is byte-identical between IDF 5.5 and 6.0.2 apart from one `#define`. Neither config nor port explains the gap.
- **TLS session resumption would not fix a cold handshake.** It helps only the second connection to a host within the ticket lifetime, and would make a repeated measurement *look* fixed while hiding the cost.

### IDF 6 / mbedTLS 4 API changes

- **IDF 6 ships mbedTLS 4.** Hash primitives moved to TF-PSA-Crypto; `<mbedtls/sha256.h>` is now `mbedtls/private/sha256.h`. Use `psa_hash_*` (`psa_crypto_init()` is idempotent). `<mbedtls/base64.h>` survives.
- **`mdns` is no longer bundled with IDF** — it is `espressif/mdns` in the component registry from 6.0 on.
- **`idf_component_register` silently ignores `SRC_DIRS` when `SRCS` is also given**, with one warning in a very long configure log.
- **IDF applies `-Werror` to warning classes arduino-esp32 did not**, including `-Wformat` and `-Wformat-truncation`. `uint32_t` is `long unsigned` here, so `%u` on a heap figure is a hard error — cast and use `%lu`.
- **A Kconfig `choice` needs the current pick explicitly unset** before the alternative takes: `# CONFIG_X_SIZE is not set` then `CONFIG_X_PERF=y`. Adding the alternative alone leaves the earlier `=y` winning, silently. Always assert the override landed in the generated `sdkconfig` before believing a measurement.
- **`esp_sntp_init()` and `esp_sntp_stop()` are both a bare `tcpip_callback`** — fire-and-forget posts. Ticket #52 was a queued stop landing after a fresh init. `Clock.cpp` reproduces Arduino's `configTzTime`: raw lwIP `sntp_*` under the lwIP core lock. **`<esp_sntp.h>` is deliberately not included** — it defines deprecated `static inline` shims that shadow `sntp_init` / `sntp_setservername` / `sntp_setoperatingmode` with the async variants, which would reintroduce #52 while every call site still read as the raw API.
- **In PsychicHttp's native mode, `uri()`, `host()`, `body()`, `getParam()->value()` and upload filenames are all `const char*`.** `==` against a literal compiles fine and compares pointers — every one needs `strcmp`. `host()` is backed by a member the next accessor reuses; copy it first. `uri()` returns `_uri.c_str()` and is stable.
- **`PsychicHttp`'s `ON_AP_FILTER`/`ON_STA_FILTER` use pure `esp_netif`** (`ESP_NETIF_DHCP_SERVER`) — but both netifs must exist, so `Net.cpp` creates them up front.

### Behaviour and hardware

- **`ESP.getFreeHeap()` was `MALLOC_CAP_INTERNAL`**, not `esp_get_free_heap_size()` (`MALLOC_CAP_DEFAULT`). They differ by ~52 KB — the IRAM-leftover region, which cannot serve a byte buffer. `Platform::freeHeap()` uses INTERNAL to stay comparable with every heap number this project has recorded, including the #119 "~48 KB free" threshold. `freeHeap8Bit()` is the pool a TLS handshake draws from. **`minFreeHeap()` tracks the 8-bit low-water mark, not `freeHeap()`'s** — compare it against `free8Bit`, never `free`.
- **`esp_ota_begin` refuses to start while the running image is `PENDING_VERIFY`.** Arduino's `Update.h` had no such constraint. Both `Ota.cpp` and `GhUpdate.cpp` confirm first.
- **`buffer_size_tx` defaults to 512 B but GitHub's signed CDN URL is ~1.2 KB**, so a redirected request line does not fit and the status sticks at 302. `core/Http` sets 2048.
- **`esp_http_client` de-chunks**; the chunked-body trap (#96) belonged to Arduino's `WiFiClient`. So `core/Http` can and must stream-parse — GitHub's release JSON will not fit the heap whole.
- **`esp_wifi_scan_get_ap_records` CONSUMES the results.** They are collected in the `SCAN_DONE` handler, as arduino-esp32 did, and served from `scanHits` afterwards. Deferring the fetch to the next HTTP poll was a real bug: "done" with zero networks.
- **`esp_wifi` has no auto-reconnect.** The old `WiFi.reconnect()` was belt-and-braces; it is now the entire reconnect policy.
- **Touch: `RAW` reads are rejected** by an upstream guard bug (`espressif/esp-idf#18811`, fixed after v6.0.2) in both 5.5.5 and 6.0.2. Read `SMOOTH` with a filter installed, and the driver's default filter is an IIR that adds felt latency — `Touch.cpp` installs a pass-through, because quick taps were being missed. Values **fall** when touched, and the scale is ~1600 where `touchRead()` gave a few hundred, so the margin is a **percentage** (untouched 1607, held ~1270, threshold 10%).
- **Do not zero-initialise IDF config structs blindly.** `touch_channel_config_t{}` sets `charge_speed` to enum 0 (the slowest), not "unset", and the pad never charges.
- **The softAP `no need to send deauth when softap is sending deauth` flood is a CLIENT, not us.** It appears only on boots where something with a saved profile pounces on the AP as it starts beaconing (a `wifi:station: <mac> join` always follows); with no client in range it does not appear at all. Counts observed: 24, 2, 0. Not a conversion regression — we `esp_wifi_set_config(WIFI_IF_AP)` *before* `esp_wifi_start()`, where arduino-esp32's `APClass::create` configured after starting, which restarts a running AP.
- **`SMOLBASE_LOOP_BUDGET_MS` is framebuffer-mode dependent** — 25 ms direct-draw, 40 ms with a full-frame buffer, where `Display::present()` alone is a blocking 25–29 ms panel push. At 25 ms a framebuffer build overran on every frame and the overrun counter was pure noise.

## Mistakes worth not repeating

Most of these were made during the sessions that produced this port. Each cost real time.

- **A heredoc mangled escape sequences four separate times.** `'\0'` became a literal NUL in a source file (caught only because `-Werror` flags "null character(s) preserved in literal"); `\n` became a real newline inside a `printf`, breaking a build; and earlier `"\xC2\xB0"` was double-encoded into `Â°C`, which nothing would have failed on. Non-ASCII or escape-heavy edits go through the editor tools, never a shell heredoc. Better still, write the code so it needs no escape.
- **I flashed a stale binary because I did not check the build's exit code** before the OTA, then puzzled over missing instrumentation. Gate every flash on the build result.
- **I wrote a causal claim into a source comment from one correlated log line** — that scanning deauthenticated the AP client — while my own polling loop was hammering that link. Correlation in a single log is not causation.
- **I reached for a workaround before earning the diagnosis**: raising the watchdog timeout instead of finding out why a handshake took 8.5 s. Pushed back on twice, and both times there was a real fix underneath.
- **I claimed session resumption would fix the handshake cost.** It would not, and it would have hidden the problem behind a flattering measurement.
- **I concluded a regression was unfixable upstream** after exhausting the config surface. Wrong: the answer was a buffered reader plus turning an accelerator *off*.
- **I called the `sdkconfig` deletion superfluous.** It is the mechanism; `sdkconfig.defaults` cannot override an existing `sdkconfig`.
- **A 112-insertion/112-deletion diff on a file you barely touched means line endings flipped.** `pathlib.write_text` translates `\n` to `\r\n` on Windows; pass `newline=""`. This repo already has mixed endings (`core.autocrlf=false`, no `.gitattributes`) so nothing breaks, but it makes noisy history — `d1d7828` and `bf05e0b` each carry a whole-file flip.
- **I called `/api/wifi/forget` while reaching for a reboot** and wiped credentials; a human had to re-provision by hand. There is no plain restart endpoint. Do not improvise one from the API list.
- **I changed how a metric was computed and then compared across the change**, and spent a long stretch hunting a 52 KB "regression" that did not exist. The ruler, not the memory.
- **I asserted an upstream bug was undiscovered**; one `gh search issues` showed it reported, confirmed and fixed. Search before concluding, especially in an unfamiliar SDK.
- **A pass condition that accepts too little evidence is worse than a failure.** The filesystem-image bug returned HTTP 200, rebooted cleanly, and served a page — and was completely broken. Only comparing *what was served* against what was built found it. The phase-0 spike made the same mistake: check 8 accepted `status > 0` and reported PASS on a 404 that never reached the CDN.

## Bugs this migration surfaced but did not cause

- `core/Http`'s `ClientReader` was **unbuffered** — `read()` fetched one byte at a time straight through mbedTLS, so GitHub's ~30 KB release JSON became ~30,000 TLS round trips, 2.07 s per fetch. Introduced by phase 4a, which replaced an Arduino `Stream` (backed by `NetworkClientRxBuffer`, a heap-allocated 1436-byte TCP-MSS buffer that exists precisely to make `Stream::read()` cheap) with a raw per-byte read. Now buffered at the same size, in the same place.
- `littlefs_create_partition_image` was pointed at `data-<app>/w`, but `littlefs-python create` lays the *contents* of its source at the image root — so the image held `/index.html.gz` while the firmware serves from `/w/`. A valid image that mounts cleanly and 404s everything. `mklittlefs` got this right by accident because PlatformIO handed it `data-<env>/`. Every release would have shipped an unusable `<app>-littlefs-<tag>.bin`.
- `Web::start()` was one-shot and lost a race against the AP netif coming up, so a device booting without credentials beaconed, served DHCP, answered the captive DNS — and had nothing on port 80. Joinable, unprovisionable, unrecoverable without the serial header.
- `jwtPayload` passed `sizeof(buf)` to `mbedtls_base64_decode` while writing a NUL at `buf[outLen]` — one past the end on a full 512-byte decode. **Still unfixed; the CGM app has never been flashed.**
- `findBackupDir` matched backups with `strncmp(path, "/w.", 3)`, assuming the volume was mounted at the root. (It is again — but by decision now, not by accident.) A backup that cannot be found never gets restored, which is the point of #122.
- `/api/update/check` compared version **strings**, so a device ahead of the latest release was offered a **downgrade**.
- `extractTo` built its output path with an `snprintf` that could truncate a 100-byte ustar member name into a 96-byte buffer, writing the wrong file. Found by `-Werror=format-truncation`.
- The weather App reported its fetch task's free stack as high-water **× 4**. ESP-IDF's `uxTaskGetStackHighWaterMark` returns bytes, not the words vanilla FreeRTOS documents — four times too generous, in the wrong direction for a number whose job is to warn before an overflow.
- `Platform::largestFreeBlock()` counted `MALLOC_CAP_INTERNAL` while documented as "what a TLS handshake actually needs" — memory that cannot serve a byte buffer.
- The weather geocoder persisted `wx_geo_name` and `wx_geo_cc` and **never read them back**, so the country code beside the city blanked on every cycle that did not re-geocode — all of them once the cache is warm, including after every reboot.
