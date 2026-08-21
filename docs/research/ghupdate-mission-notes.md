# GitHub Release OTA — Mission Notes

> **ADDENDUM 2026-08-21 (wayfinder map #112).** The pipeline documented below was reviewed, measured, and **replaced** — `GhUpdate.cpp` now uses raw `esp_https_ota` on the deterministic release URL, spawned directly from the POST handler (no `tick()`), after waiting for the `OtaStarting` heap plateau. Corrections to the notes below, established on-device (#116/#119):
>
> - **The CDN host moved**: release assets now come from `release-assets.githubusercontent.com` (Azure-blob backed), not `objects.githubusercontent.com`, served with a Let's Encrypt chain (leaf ← YR1 ← ISRG Root YR **cross-signed by RSA-4096 ISRG Root X1**).
> - **That chain broke the shipped OTA in the field**: the ESP32 RSA accelerator cannot complete 4096-bit verifies (`CONFIG_MBEDTLS_HARDWARE_MPI=y`, `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` not set in the precompiled libs → PK error `0x4290`, surfacing as `-0x3000 X509_FATAL`). Fixed via pioarduino `custom_sdkconfig` hybrid compile enabling the software-MPI fallback (plus `OUT_CONTENT_LEN=4096`, −12 KB per TLS connection).
> - **One live bundle-TLS session costs ~50 KB**, not ~40 KB (measured 92 → 42 KB across a handshake); at low heap the same chain also dies earlier with MPI alloc errors, which masqueraded as "network flakiness" on steps 1/2.
> - **The task stack high-water was ~4.9 KB** of the 16 KB provisioned; the rewrite uses 8 KB.
> - Pitfalls 3, 5, 6, 7 below are **moot** in the esp_https_ota pipeline (no Update.begin ordering, no readBytes stall, no manual redirect); pitfalls 1 (Core 0 pinning), 2 (heap-sized stack), and 4 (vTaskDelete skips destructors) still stand and are honored by the new code.


**Device:** GeekMagic Small TV Pro (ESP32-D0WD, 8 MB flash, no PSRAM)  
**Platform:** pioarduino / arduino-esp32 3.3.11 / ESP-IDF 5.5.5  
**Goal:** device running 0.3.0 self-updates to 0.3.1 from a GitHub release via the settings page.

---

## Architecture that landed

```
POST /api/update/github  (httpd task, Core 0)
  → sets s_pendingBuf + s_hasPending

GhUpdate::tick()  (Arduino loop, Core 1)
  → sees s_hasPending
  → xTaskCreatePinnedToCore(downloadTask, core=0, stack=16KB, prio=5)

downloadTask  (Core 0)
  → downloadImpl()  ← all network + flash, normal C++ function
  → vTaskDelete(nullptr)  ← destructors already ran; no heap leak

downloadImpl():
  1. Update.begin(UPDATE_SIZE_UNKNOWN)  ← FIRST, before any TLS
  2. BundleClient + HTTPClient: GET api.github.com release JSON → assetApiUrl
  3. Same objects: GET assetApiUrl → 302 → save cdnUrl
  4. Same objects: GET cdnUrl → 200 → stream body into Update
  5. Update.end(true)
```

---

## Pitfalls hit, in order

### 1. WDT at ~8 s on Core 1

Tried running the HTTPS download in the Arduino loop task (Core 1). Crashed at ~8 s every time — WDT or an lwIP/IPC timing issue with HTTPS from Core 1. Root cause never pinned down, but the pattern is consistent: HTTPS works reliably from Core 0 (the httpd task proves it), not from Core 1.

**Fix:** pin the download task to Core 0.

### 2. Task creation failed (OOM)

First attempt used a 64 KB task stack. Device only has ~103 KB free heap at boot and allocations are fragmented — no contiguous 64 KB block. Task creation returned `pdFAIL`.

**Fix:** 16 KB stack. TLS I/O buffers (~32 KB) are heap-allocated by mbedTLS, not stack. The stack only needs to cover call depth.

### 3. Cross-core String visibility (red herring)

Used `static String s_pendingTag` for the POST → tick() handoff. Suspected cross-core visibility issues because the download never started. Turned out the device was rebooting (WDT), not ignoring the flag. Diagnosed via the tick counter in `/api/update/ghprogress` dropping between polls.

**Fix:** Changed to `static char s_pendingBuf[32]` + `static volatile bool s_hasPending` to be explicit, but the real fix was Core 0 pinning (pitfall 1).

### 4. TLS heap leak → vTaskDelete skipping destructors

After moving to Core 0, the download task was calling `vTaskDelete(nullptr)` throughout `downloadTask()` (e.g. in the fail lambda). `vTaskDelete` immediately terminates the task — C++ destructors for any live locals **do not run**. The `BundleClient` TLS context (~40 KB) leaked every attempt.

Confirmed by watching heap drop from 102 KB → 58 KB after one OTA attempt. A subsequent manual OTA upload then failed with "no file uploaded" because 58 KB wasn't enough.

**Fix:** RAII wrapper pattern:
```cpp
static void downloadTask(void* arg) {
    bool ok = downloadImpl(arg);   // all work here; destructors run on return
    if (ok) { ...; Net::restartToApply(); }
    vTaskDelete(nullptr);          // only called after cleanup
}
```

### 5. `Update.begin()` returning false with `errorString() == "No Error"`

This is the nastiest pitfall. `Update.begin()` has **two** silent failure paths in arduino-esp32 3.x — both return `false` without setting `_error`:

```cpp
// Path A (line ~187):
if (_size > 0) { return false; }           // already running — no error set

// Path B (line ~291):
_buffer = new (std::nothrow) uint8_t[SPI_FLASH_SEC_SIZE];  // 4096 bytes
if (!_buffer) { return false; }            // OOM — also no error set
```

`Update.errorString()` returns `"No Error"` for both. `isRunning()` returns `_size > 0`, so after a failed `begin()` it also returns false (no stale state to clean up with `abort()`). The `"No Error"` response masks both conditions.

**Root cause here:** Path B — the 4 KB `_buffer` allocation was failing because `BundleClient` (~40 KB TLS context) + task stack (16 KB) had consumed most of the heap before `begin()` was even called.

**Fix:** call `Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)` at the **very top** of `downloadImpl()`, before constructing `BundleClient` or `HTTPClient`. At that point only the FreeRTOS task stack is allocated — ~86 KB remains, and 4 KB trivially succeeds.

`UPDATE_SIZE_UNKNOWN` sets `_size = partition->size` (2.125 MB) so the partition is reserved but the exact firmware size doesn't need to be known upfront. `end(true)` finalises whatever was written.

### 6. Download stall at a fixed byte count

With `Update.begin()` fixed, the download started but stalled at a consistent byte count before completion. The count changed based on the read strategy:

| Read strategy | Stall point |
|---|---|
| `readBytes()` + `vTaskDelay(1)` after every 4 KB chunk | 10,968 bytes |
| `readBytes()` + `vTaskDelay(1)` every 64 KB | 32,768 bytes |

32,768 = exactly 2 × 16,384 — two complete TLS records at `MBEDTLS_SSL_IN_CONTENT_LEN = 16384`. The pattern is: CDN sends exactly enough data to fill the TCP receive window, then stops and waits for a window update. lwIP is ready to send the ACK/window update but our task is blocking Core 0 inside `readBytes()` for the full stream timeout (30–120 s) without yielding. The CDN eventually times out the idle connection.

**Why `readBytes()` blocks:** `Stream::readBytes()` calls `timedRead()` in a byte-at-a-time loop. When no byte is immediately available, `timedRead()` blocks the calling task until the stream timeout expires. While blocked, the task holds Core 0. The lwIP `tcpip_thread` (also Core 0, priority 18) can technically preempt, but in practice the window update races with the CDN's server-side idle timeout.

**Fix:** replace `readBytes()` with a polling loop:

```cpp
while (netStream->available() <= 0) {
    if (!netStream->connected()) { ... }
    if (millis() - t0 > 120000) { ... }
    vTaskDelay(pdMS_TO_TICKS(10));      // yield — lets lwIP send window update
}
int rd = netStream->read(buf, min(remaining, 4096));
```

`NetworkClientSecure::available()` calls `mbedtls_ssl_read(ctx, nullptr, 0)` internally — a zero-byte read that drives the mbedTLS state machine and drains pending socket data. The 10 ms `vTaskDelay` between probes gives lwIP time to send the TCP ACK, the CDN receives the window update, and streaming resumes.

### 7. HTTPC_FORCE_FOLLOW_REDIRECTS breaks body delivery (dead end)

At one point `HTTPC_FORCE_FOLLOW_REDIRECTS` was used so HTTPClient would handle the github.com → CDN redirect internally. This produced `conn=1` (TCP connection alive) but 0 bytes readable — the CDN body was unreachable after HTTPClient's internal redirect reconnected TLS on the same object. Not fully diagnosed, but this approach was abandoned.

**Fix:** manual redirect: disable follow-redirects for the asset API step, capture the CDN URL from the `Location` header, then GET that URL directly with `HTTPC_DISABLE_FOLLOW_REDIRECTS`. One less TLS reconnection, no HTTPClient redirect magic to go wrong.

### 8. `http.setTimeout()` overflow

`HTTPClient::setTimeout(uint16_t)` takes `uint16_t`, max 65535 ms. Passing 120000 compiled without error (implicit narrowing) but silently truncated to 54464 ms. The compiler warning (`-Woverflow`) caught it.

**Fix:** use 60000 for the HTTPClient timeout. `tls.setTimeout()` (from `Stream`) takes `unsigned long` so 120000 is fine there.

---

## Key invariants to know

- **`Update.begin()` must precede all TLS allocation.** The 4 KB sector buffer allocation happens inside `begin()` and silently fails with "No Error" if heap is fragmented. On this device, BundleClient alone costs ~40 KB.

- **`vTaskDelete()` skips destructors.** If you call it anywhere inside a function that has live C++ objects, those objects leak. Wrap all work in a helper function and only call `vTaskDelete` after it returns.

- **HTTPS works from Core 0, not Core 1.** The loop() task (Core 1) crashes making HTTPS at ~8 s. Custom tasks on Core 1 also fail TCP. Use `xTaskCreatePinnedToCore(..., 0)`.

- **`isRunning()` = `_size > 0`, not a `_running` bool.** After a failed `begin()` via path B (OOM), `_size` stays 0 (set by `_reset()` at entry), so `isRunning()` is false and `abort()` is a no-op. You can call `begin()` again directly.

- **`available()` pumps mbedTLS input.** Don't block in `readBytes()` for streaming downloads. Use `available()` in a yield loop — it drives the TLS state machine and keeps the TCP window open.

- **Heap at boot: ~128 KB.** Budget:
  - FreeRTOS task stack (16 KB): −16 KB → ~112 KB
  - `Update._buffer` (4 KB): −4 KB → ~108 KB  
  - BundleClient TLS context (≈40 KB): −40 KB → ~68 KB
  - mbedTLS active I/O buffers (≈32 KB, dynamic): −32 KB → ~36 KB during active TLS record read
  - Download write buffer (4 KB): −4 KB → ~32 KB minimum floor during streaming
