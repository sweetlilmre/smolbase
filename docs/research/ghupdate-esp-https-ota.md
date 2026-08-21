# esp_https_ota as a replacement for the BundleClient + HTTPClient + Update pipeline

Research for issue #114. Question: on arduino-esp32 3.3.11 / ESP-IDF 5.5.5 (pioarduino, precompiled IDF libs, ESP32-D0WD, 8 MB flash, no PSRAM), can `esp_https_ota` replace the current `BundleClient(NetworkClientSecure)` + `HTTPClient` + `Update` download-and-flash path in `src/core/GhUpdate.cpp` for GitHub release assets?

All IDF source citations are against the exact shipped version: tag `v5.5.5` of espressif/esp-idf (the precompiled libs report `esp-idf: v5.5.5 b774170ff46` in `~/.platformio/packages/framework-arduinoespressif32-libs/versions.txt`; the compiled binaries come from Espressif's fork at that commit, which tracks v5.5.5 — treat upstream v5.5.5 as authoritative modulo Arduino-specific patches). Local file citations are against `C:\Users\petere\.platformio\packages\framework-arduinoespressif32` (3.3.11) and `framework-arduinoespressif32-libs` (the precompiled esp32 libs actually linked).

## Verdict

**Yes — viable and a clear net win, using the raw `esp_https_ota_begin`/`perform`/`finish` API directly, NOT Arduino's `HttpsOTAUpdate` wrapper (which is unusable here: no cert-bundle support, no progress bytes, unpinned task, and an uninitialized-struct bug).** Everything needed is present in pioarduino's precompiled libs: `libesp_https_ota.a` is shipped, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` is baked in, `crt_bundle_attach` is a plain config-struct field, and `esp_http_client` natively follows the api.github.com → 302 → objects.githubusercontent.com redirect while preserving the `Accept: application/octet-stream` header. It writes straight to the OTA partition via `esp_ota_write` (no 4 KB `Update.cpp` sector buffer, no 4 KB app write buffer), reads via `select()` (no TCP-window stall, so the hand-rolled `available()` poll loop dies), and adds free chip-id/revision validation of the image header. Expected saving: roughly 8 KB heap during streaming plus ~250 lines of fragile hand-rolled download code; TLS cost is unchanged (~40 KB, one mbedTLS context with 2×16 KB I/O buffers — same mbedTLS, same sdkconfig). Keep the existing Core 0 task (16 KB stack is comfortably above the 8 KB the IDF example uses), keep step 1 (release JSON → asset API URL) on the existing HTTPClient path or esp_http_client, and hand esp_https_ota the asset API URL with headers set via `http_client_init_cb`. Do NOT enable `partial_http_download` (its HEAD preflight breaks on the 302) and don't bother with `ota_resumption`.

## 1. Cert-bundle support — yes, fully reachable

`esp_http_client_config_t` has the field `esp_err_t (*crt_bundle_attach)(void *conf)` — present in the precompiled header at `framework-arduinoespressif32-libs/esp32/include/esp_http_client/include/esp_http_client.h:211`. `esp_http_client.c` wires it into the TLS transport, gated at compile time on the bundle Kconfig: `if (config->crt_bundle_attach != NULL) { #ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE esp_transport_ssl_crt_bundle_attach(ssl, config->crt_bundle_attach); ... }` (https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_http_client/esp_http_client.c lines 836–841).

The precompiled sdkconfig (`framework-arduinoespressif32-libs/esp32/sdkconfig`) has `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` (full Mozilla bundle, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS=200`) — the same bundle the current `BundleClient` uses via `attach_ssl_certificate_bundle()`. The header `esp_crt_bundle.h` is shipped at `framework-arduinoespressif32-libs/esp32/include/mbedtls/esp_crt_bundle/include/esp_crt_bundle.h`, so a sketch can `#include "esp_crt_bundle.h"` and set `.crt_bundle_attach = esp_crt_bundle_attach` directly. `libesp_https_ota.a`, `libesp_http_client.a`, `libesp-tls.a` are all in `framework-arduinoespressif32-libs/esp32/lib/`, so no rebuild of IDF is needed.

esp_https_ota actively *requires* one of `cert_pem` / `use_global_ca_store` / `crt_bundle_attach` unless `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` is set: `is_server_verification_enabled()` and the `ESP_ERR_INVALID_ARG` bail-out (https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_https_ota/src/esp_https_ota.c lines 297–325). The precompiled sdkconfig has `# CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP is not set`, so bundle attach is mandatory — which is what we want anyway.

## 2. Redirect handling — native, headers preserved

esp_https_ota handles redirects itself inside `_http_connect()`: after `esp_http_client_fetch_headers`, `_http_handle_response_code()` calls `esp_http_client_set_redirection()` for 301/302/303/307/308, drains the redirect response body, and the `do { } while (process_again(status_code))` loop re-opens against the new URL (esp_https_ota.c lines 71–218). The header comment confirms: "This API supports URL redirection" (shipped header `esp_https_ota.h:91` and `:121`).

**Custom headers survive the redirect, including the cross-host hop.** `esp_http_client_set_redirection()` → `esp_http_client_set_url()` (esp_http_client.c:1093), and `esp_http_client_set_url()` only rewrites `connection_info` (host/port/path/query/scheme), updates the `Host` header, and closes the socket when host or port changed (esp_http_client.c lines 1146–1249). It never touches the user header list — there is no header-stripping code on the redirect path in v5.5.5 (`esp_http_client_prepare()` clears only `location`, `auth_header`, and response state, esp_http_client.c:680–700). So `Accept: application/octet-stream` and `User-Agent` set once via `esp_http_client_set_header()` are re-sent to objects.githubusercontent.com. (Corollary: an `Authorization` header would also be forwarded cross-host — irrelevant for this public repo, but don't add a GitHub token this way.)

That means the flow collapses to: give esp_https_ota the **asset API URL** (`https://api.github.com/repos/.../releases/assets/{id}`), set the `Accept` and `User-Agent` headers in the `http_client_init_cb` hook (field at shipped `esp_https_ota.h:63`, invoked right after `esp_http_client_init` at esp_https_ota.c:372–378 — this is exactly the documented way to inject headers, since `esp_https_ota_config_t` has no header field of its own), and the 302 → CDN → 200 → stream happens inside `esp_https_ota_begin()`. Redirect targets are restricted to https-on-https (`ESP_ERR_HTTP_REDIRECT_DOWNGRADE` guard, esp_http_client.c:1081–1091) — fine, the GitHub CDN is https.

`max_redirection_count` defaults to `DEFAULT_MAX_REDIRECT = 10` when the config field is 0 (esp_http_client.c:167 and 531–533). GitHub's chain is 1 hop; even the occasional double-hop is far under the cap. `esp_https_ota_get_status_code()` exposes the final status if diagnostics are wanted (shipped `esp_https_ota.h:282`).

This retires mission-notes pitfall 7 (HTTPC_FORCE_FOLLOW_REDIRECTS dead end, `ghupdate-mission-notes.md:115–119`): that was an Arduino `HTTPClient` reconnect bug; esp_http_client's redirect path is the code Espressif's own OTA examples exercise against exactly this kind of CDN redirect.

## 3. Memory profile — TLS unchanged, ~8 KB less app-side buffering

- **esp_https_ota upgrade buffer:** `alloc_size = MAX(http_config->buffer_size, DEFAULT_OTA_BUF_SIZE)` where `DEFAULT_OTA_BUF_SIZE = 1024` (esp_https_ota.c:22–26 and 516–521), heap-allocated once in `esp_https_ota_begin`. With `buffer_size = 4096` that's one 4 KB buffer — replacing BOTH the current 4 KB app write buffer (`GhUpdate.cpp:193`) and the role of Update's staging.
- **esp_http_client rx/tx buffers:** default `DEFAULT_HTTP_BUF_SIZE (512)` each when `buffer_size`/`buffer_size_tx` are 0 (esp_http_client.h:19, esp_http_client.c:523–529). Setting `buffer_size = 4096` sizes the rx buffer to 4 KB (it doubles as the OTA buffer size above); tx can stay 512. Note the rx buffer is an *additional* allocation on top of the upgrade buffer, plus a ~256 B stack scratch for draining redirect bodies (esp_https_ota.c:147).
- **TLS: identical to today.** esp_http_client rides esp-tls/mbedTLS with the same precompiled sdkconfig: `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` and `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` not set (`framework-arduinoespressif32-libs/esp32/sdkconfig`), so the handshake still allocates the ~40 KB context including 2×16 KB I/O buffers. No saving, no regression — it's the same mbedTLS the current BundleClient uses.
- **No Update.cpp sector buffer.** esp_https_ota writes via `esp_ota_write()` directly (`_ota_write`, esp_https_ota.c:276–295) with `OTA_WITH_SEQUENTIAL_WRITES` incremental erase (esp_https_ota.c:690–693), bypassing Arduino `Update` entirely — the 4 KB `_buffer = new uint8_t[SPI_FLASH_SEC_SIZE]` and its infamous silent-OOM "No Error" failure path (mission-notes pitfall 5) simply cease to exist. The handle struct itself is a small `calloc` (esp_https_ota.c:337).
- **Task stack:** Espressif's own advanced_https_ota example task runs at 8 KB (`xTaskCreate(&ota_example_task, "ota_example_task", 1024 * 8, ...)`, https://github.com/espressif/esp-idf/blob/v5.5.5/examples/system/ota/advanced_https_ota/main/advanced_https_ota_example.c); Arduino's HttpsOTAUpdate uses 9216 (`framework-arduinoespressif32/libraries/Update/src/HttpsOTAUpdate.cpp:21`). The existing 16 KB Core 0 task is more than enough and can stay as-is.

Net during streaming vs. the current budget in `ghupdate-mission-notes.md:141–146`: TLS ~40 KB (same), task stack 16 KB (same), then ~4.5 KB (upgrade buf 4 KB + rx 512 + tx 512) instead of ~8 KB (Update `_buffer` 4 KB + write buf 4 KB) plus HTTPClient/String overhead — call it **~4–8 KB more headroom at the peak**, and one fewer large allocation to fragment the heap. The heap-ordering constraint from pitfall 5 also relaxes: there is no silent-failure allocation to front-load, and `esp_https_ota_begin` returns a real `ESP_ERR_NO_MEM` if the buffer can't be allocated (esp_https_ota.c:522–525).

**Bonus: no TCP-window stall.** The stall bug (mission-notes pitfall 6) came from `Stream::readBytes` busy-holding Core 0. `esp_http_client_read` reads through the tcp_transport layer whose `base_poll_read` blocks in `select()` with the client timeout (https://github.com/espressif/esp-idf/blob/v5.5.5/components/tcp_transport/transport_ssl.c lines 159–189, dispatched from `ssl_read` at 261–266) — the task genuinely sleeps, lwIP's tcpip_thread runs, window updates go out. The whole `available()` + `vTaskDelay(10)` poll loop and its 120 s stall watchdog become unnecessary. A read timeout surfaces as `-ESP_ERR_HTTP_EAGAIN`, which `esp_https_ota_perform` maps to "call me again" (esp_https_ota.c:795–798).

## 4. Progress hooks — sufficient for the polled endpoint

The perform-loop API is exactly shaped for `/api/update/ghprogress`: call `esp_https_ota_begin`, then loop `esp_https_ota_perform` while it returns `ESP_ERR_HTTPS_OTA_IN_PROGRESS` (one HTTP read + one flash write per call, "returns after every HTTP read operation thus giving you the flexibility to stop OTA operation midway", shipped `esp_https_ota.h:142–163`). Between iterations:

- `esp_https_ota_get_image_len_read(handle)` → bytes written so far (`handle->binary_file_len`, esp_https_ota.c:951–961) — maps to `s_progress.bytesWritten`.
- `esp_https_ota_get_image_size(handle)` → total from the final response's Content-Length (`esp_http_client_get_content_length` captured after redirects at esp_https_ota.c:477–478, getter at 963–973; returns -1 for chunked encoding, but the GitHub CDN sends Content-Length — the current code already hard-depends on that at `GhUpdate.cpp:180–184`) — maps to `s_progress.totalBytes`.

The Core 0 task updates the same `Progress` struct fields from inside the loop; the polled endpoint doesn't change at all. `esp_https_ota_is_complete_data_received()` (esp_https_ota.c:845–855) gives a truncation check before `esp_https_ota_finish()`, and `esp_https_ota_abort()` is the error-path cleanup (frees buffer, closes client, aborts the esp_ota handle — esp_https_ota.c:905–940). There are also `ESP_HTTPS_OTA_WRITE_FLASH` events carrying the byte count on the default event loop (esp_https_ota.c:289) if an event-driven style were ever wanted; the polling getters are simpler and enough.

## 5. Partial download / resume / ALLOW_HTTP — leave all off

- `partial_http_download` (range-request chunked download, config field at shipped `esp_https_ota.h:65–66`, `max_http_request_size` default 64 KB via `DEFAULT_REQUEST_SIZE`, esp_https_ota.c:30, 345) **must stay false for GitHub asset URLs**: it starts with a HEAD request and hard-fails unless the status is exactly 200 (`if (status != HttpStatus_Ok) ... ESP_FAIL`, esp_https_ota.c:399–412) — the asset API URL answers HEAD with 302, so begin() would die before the redirect logic ever ran. Additionally, `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD` (which gates persistent-connection reuse between range requests, esp_https_ota.c:176–187) does not exist in the v5.5.5 Kconfig (https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_https_ota/Kconfig defines only `ESP_HTTPS_OTA_DECRYPT_CB`, `ESP_HTTPS_OTA_ALLOW_HTTP`, `ESP_HTTPS_OTA_EVENT_POST_TIMEOUT`; the symbol only appears on master) — so even if partial download worked, every 64 KB chunk would tear down and re-handshake TLS. Pointless here; the streaming path is one connection per hop.
- `ota_resumption` (resume across reboots, shipped `esp_https_ota.h:68–69`) sends `Range: bytes=N-` and requires the app to persist `ota_image_bytes_written` itself (esp_https_ota.c:355–397); it's also rejected by the one-shot `esp_https_ota()` API (esp_https_ota.c:982–985). For a ~1.5 MB firmware on a stable LAN this is complexity with no payoff — skip.
- `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` is `not set` in the precompiled sdkconfig and is baked into `libesp_https_ota.a` (it gates a compile-time branch, esp_https_ota.c:318–324) — plain-HTTP OTA is impossible without swapping the precompiled libs. Not needed: everything is HTTPS with the bundle.
- `bulk_flash_erase = false` keeps the current incremental-erase behavior (`OTA_WITH_SEQUENTIAL_WRITES`, esp_https_ota.c:690) — do not enable bulk erase; erasing the whole 2 MB partition up front stalls the flash for seconds.

## 6. Pitfalls on no-PSRAM ESP32, and why HttpsOTAUpdate is disqualified

**`HttpsOTAUpdateClass` (arduino-esp32 3.3.11, `libraries/Update/src/HttpsOTAUpdate.{h,cpp}`) is too limited — confirmed against source:**

- `begin(const char *url, const char *cert_pem, bool skip_cert_common_name_check)` takes a single URL + a single PEM cert (`HttpsOTAUpdate.h:25`). It never sets `crt_bundle_attach`, so with GitHub's two hosts (api.github.com is DigiCert-rooted; objects.githubusercontent.com is Sectigo/USERTrust-rooted, per the current working BundleClient behavior) you'd have to concatenate root PEMs into `cert_pem` and keep them updated by hand — exactly what the cert bundle exists to avoid.
- It spawns `xTaskCreate(&https_ota_task, "https_ota_task", 9216, ...)` — **unpinned** (`HttpsOTAUpdate.cpp:106`). This repo's hard-won invariant is that HTTPS only survives on Core 0 (`ghupdate-mission-notes.md:135`); an unpinned task can be scheduled on Core 1.
- Progress reporting is a four-state enum only (`HTTPS_OTA_IDLE/UPDATING/SUCCESS/FAIL`, `HttpsOTAUpdate.h:14–20`); no byte counts, because it uses the one-shot `esp_https_ota()` API internally (`HttpsOTAUpdate.cpp:54`). The polled progress endpoint would regress to a spinner.
- Latent bug: `esp_https_ota_config_t cfg;` is a stack local whose `buffer_caps`, `ota_resumption`, `ota_image_bytes_written`, and the whole `partition` sub-struct are **left uninitialized** (`HttpsOTAUpdate.cpp:45–52` sets only `http_config`, `http_client_init_cb`, `bulk_flash_erase`, plus partial fields under an `#if` that is false on 5.5.5). `esp_https_ota_begin` then reads `ota_config->partition.staging` (esp_https_ota.c:488) and `buffer_caps` (esp_https_ota.c:517) as stack garbage. Whether it happens to work is luck. Do not build on this class.

**No-PSRAM pitfalls for the raw API (all manageable):**

- The TLS handshake peak is unchanged (~40 KB), so the same discipline applies: start the OTA task when heap is healthy (~100 KB) and never hold a second TLS context concurrently. The version-check endpoint (`detectLatestTag`) must not run while the download is in flight — the existing `s_inFlight` guard already covers this (`GhUpdate.cpp:47, 325`).
- `buffer_caps` should stay 0 (`MALLOC_CAP_DEFAULT`) — it exists mainly to force PSRAM placement on boards that have it (shipped `esp_https_ota.h:67`); on this device there is nothing else to select.
- The task WDT is enabled with panic (`CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, watching the Core 0 idle task per `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`, precompiled sdkconfig). A priority-5 task blocked in `select()` yields properly, so the idle task starves only if `esp_https_ota_perform` is hammered in a zero-wait spin while data is always available; in practice each iteration does a flash write (which blocks on the flash op) and the loop matches Espressif's own example (`while (1) { err = esp_https_ota_perform(...); if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break; ... }`, advanced_https_ota_example.c). If WDT noise ever shows up, a `vTaskDelay(1)` every N iterations is safe here because reads block in select rather than racing the TCP window — the pitfall-6 failure mode does not recur.
- Keep step 1 (resolving tag → asset API URL from the release JSON) exactly as it is, or port it to esp_http_client later; esp_https_ota only replaces steps 2+3 of the current three-step dance, and steps 2+3 collapse into one call. Chip-id/revision verification of the downloaded header comes free (`esp_ota_verify_chip_id`/`esp_ota_verify_chip_revision`, esp_https_ota.c:653–674, 742–752) — protection the current pipeline lacks against flashing an image built for the wrong chip.
- After `esp_https_ota_finish()` returns ESP_OK the boot partition is already switched (esp_https_ota.c:887–898); just keep the existing 3 s grace delay and `Net::restartToApply()`.

## 7. What switching saves and costs on this device

**Saves:**

- ~4–8 KB heap at the streaming peak (no Update 4 KB sector buffer, no separate 4 KB write buffer, no HTTPClient String overhead; replaced by one 4 KB upgrade buffer + 512 B rx/tx pair) — on a device whose recorded floor is ~32 KB (`ghupdate-mission-notes.md:146`), that's meaningful margin.
- The entire hand-rolled download machinery: manual redirect capture, the `available()`/`vTaskDelay` anti-stall loop, the stall watchdog, the `Update.begin()` heap-ordering workaround, and the two silent `Update.begin()` failure modes (pitfalls 5, 6, 7 all become moot). `downloadImpl()` shrinks from ~170 lines to roughly 40.
- Free image sanity checks (chip id, chip revision, app-descriptor magic via optional `esp_https_ota_get_img_desc`) before most of the partition is burned.
- Real error codes (`ESP_ERR_NO_MEM`, `ESP_ERR_OTA_VALIDATE_FAILED`, `ESP_ERR_HTTP_MAX_REDIRECT`, ...) instead of `"No Error"`.

**Costs / unchanged:**

- TLS heap is identical (~40 KB context, 2×16 KB I/O buffers) — the bundle, mbedTLS, and sdkconfig are shared with the current path.
- Still needs the dedicated Core 0 task and the same `s_inFlight`/`s_hasPending` handoff; esp_https_ota is blocking by design (`is_async` is explicitly rejected, shipped `esp_https_ota.h:129–130`). The 16 KB stack stays (could likely drop to 12 KB after soak testing, given the 8–9 KB reference points, but there's no pressure to).
- One new failure surface: error mapping from `esp_err_t` to the progress endpoint's `errorMsg` (trivial via `esp_err_to_name`).
- The C-style begin/perform/finish handle needs the same RAII discipline as pitfall 4 — wrap it so `esp_https_ota_abort()` runs on every early-out before `vTaskDelete` (a small scope-guard struct does it).
- Behavioral note: `Update.onProgress` UI hooks (if any were planned) don't exist; the polled getters replace them.

**Recommended shape** (unverified on hardware — flashing is user-gated; this is the design the sources support):

```cpp
esp_err_t initCb(esp_http_client_handle_t h) {
  esp_http_client_set_header(h, "Accept", "application/octet-stream");
  esp_http_client_set_header(h, "User-Agent", "smolbase-esp32");
  return ESP_OK;
}
// in downloadImpl, on the Core 0 task:
esp_http_client_config_t http{};
http.url = assetApiUrl.c_str();            // api.github.com/.../assets/{id}
http.crt_bundle_attach = esp_crt_bundle_attach;
http.timeout_ms = 30000;
http.buffer_size = 4096;                    // rx buf + upgrade buf size
esp_https_ota_config_t ota{};               // value-init: all fields zeroed
ota.http_config = &http;
ota.http_client_init_cb = initCb;
esp_https_ota_handle_t h = nullptr;
ESP_ERROR_CHECK_WITHOUT_ABORT(esp_https_ota_begin(&ota, &h)); // handles 302 → CDN
s_progress.totalBytes = esp_https_ota_get_image_size(h);
while (esp_https_ota_perform(h) == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
  s_progress.bytesWritten = esp_https_ota_get_image_len_read(h);
if (esp_https_ota_is_complete_data_received(h) && esp_https_ota_finish(h) == ESP_OK)
  /* done → restart */;
else esp_https_ota_abort(h);
```

## Source index

- Shipped precompiled sdkconfig: `~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig` (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`, `# CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP is not set`, `# CONFIG_ESP_HTTPS_OTA_DECRYPT_CB is not set`, `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, task-WDT block)
- Shipped headers: `.../esp32/include/esp_https_ota/include/esp_https_ota.h`, `.../esp_http_client/include/esp_http_client.h`, `.../mbedtls/esp_crt_bundle/include/esp_crt_bundle.h`; libs in `.../esp32/lib/` (`libesp_https_ota.a`, `libesp_http_client.a`, `libesp-tls.a`)
- IDF v5.5.5 sources: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_https_ota/src/esp_https_ota.c · https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_http_client/esp_http_client.c · https://github.com/espressif/esp-idf/blob/v5.5.5/components/tcp_transport/transport_ssl.c · https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_https_ota/Kconfig · https://github.com/espressif/esp-idf/blob/v5.5.5/examples/system/ota/advanced_https_ota/main/advanced_https_ota_example.c
- Arduino wrapper (3.3.11): `~/.platformio/packages/framework-arduinoespressif32/libraries/Update/src/HttpsOTAUpdate.h` / `HttpsOTAUpdate.cpp`
- Current implementation and pitfall log: `src/core/GhUpdate.cpp`, `ghupdate-mission-notes.md`
