# GhUpdate deep review — architecture, memory anatomy, leanness

Wayfinder ticket [#113](https://github.com/sweetlilmre/smolbase/issues/113), map [#112](https://github.com/sweetlilmre/smolbase/issues/112). Code under review: `src/core/GhUpdate.{h,cpp}` at commit `2c3599b`, plus its touchpoints (`src/main.cpp:74`, `src/core/Web.cpp:303`, `html/settings.html` GitHub-update section). Companion background: `ghupdate-mission-notes.md` (repo root). This is a code-reading review with a few empirical HTTP checks; on-device numbers are ticket #116's job.

## Summary of findings

1. **The Core-0 dedicated-task shape is right, but the `tick()` handoff is vestigial.** The POST handler already runs on Core 0 (httpd task); it can call `xTaskCreatePinnedToCore(..., 0)` directly. The whole Core-1 relay — `GhUpdate::tick()` in `loop()`, `s_hasPending`, `s_pendingBuf`-as-cross-core-mailbox, `s_tickCount` diagnostics — exists only because the first design ran the download in `loop()`. Deleting it removes a per-loop-iteration call, two volatile globals, a real (if tiny) double-queue race, and up to one loop-latency of delay, at zero cost.
2. **Step 1 (release JSON fetch + parse) is deletable.** The firmware asset URL is deterministic from the tag: `https://github.com/<repo>/releases/download/<tag>/smolbase-firmware-<tag>.bin` answers 302 → CDN with no special headers (verified 2026-08-21 with a plain GET, no `Accept`, no auth). That removes one full HTTPS request to api.github.com, the ArduinoJson filter+doc pair, the asset-name loop, and the `assetApiUrl` String — the pipeline becomes GET github.com → 302 → GET CDN → flash.
3. **The header handling works by accident and one call is a no-op.** Three review-relevant facts from arduino-esp32 3.3.11 `HTTPClient.cpp`:
   - `addHeader("User-Agent", ...)` is silently **filtered out** (`HTTPClient.cpp:994` excludes `Connection`, `User-Agent`, `Accept-Encoding`). Both call sites (`GhUpdate.cpp:61,111`) are dead code; the requests ride on the default `_userAgent = "ESP32HTTPClient"` (`HTTPClient.h:303`), which GitHub accepts (a UA-less request gets 403 — verified empirically).
   - Every `begin()` overload calls `clear()` (which wipes `_headers`) **except** `begin(NetworkClient&, String url)` → `beginInternal()` — the one overload this code uses. That's the only reason the `Accept:` headers added *before* `begin()` are actually sent. Switching overloads would silently break step 2's 302 (without `Accept: application/octet-stream` the asset API returns 200 JSON — verified). Deleting step 1 (finding 2) sidesteps most of this fragility.
   - `end()` calls `clear()`, so headers never leak between steps — the top-of-function `addHeader` calls only ever applied to step 1.
4. **Connection reuse across steps is real and worth keeping.** Steps 1→2 share host `api.github.com`; `beginInternal` only disconnects on host change, and `end()`/`disconnect(false)` honors keep-alive (`_reuse && _canReuse`), so the shipped code does two handshakes (api.github.com, CDN), not three. With step 1 deleted it's still two (github.com, CDN) — but the api.github.com handshake churn disappears from `/api/update/check`'s already-heavy neighborhood.
5. **`/api/update/check` is the heaviest steady-state-visible cost.** `detectLatestTag()` builds a full `BundleClient` (TLS context + cert-bundle verification, ~40 KB transient plus mbedTLS I/O buffers) **on the 8 KB-stack httpd task** per click, and blocks that task (all HTTP, all pages, all polls) for up to 10 s. Heap is fine (TLS is heap-allocated, not stack), but the blocking and the transient spike while apps hold heap are the real exposure. It's on-demand-only (button), which is why it's been acceptable. Leanest fix that preserves behavior: nothing structural — just note it; optionally shorten timeouts. Moving it to a worker task buys reentrancy the UI doesn't need.
6. **The UI feedback mechanism is already near the floor.** `/api/update/ghprogress` is a 200-byte stack `snprintf` of five scalars — no heap, no String, no JSON lib. The browser polls at 1 Hz (`settings.html:523-551`). Cost during download: one small request/second served by the httpd task, which shares Core 0 with the download task at equal priority (httpd default 5 == download task 5), so they timeslice; the download loop's `vTaskDelay(10)` idle-poll gaps leave plenty of room. Cheaper alternatives don't exist in kind: SSE/WebSocket would hold a persistent PsychicHttp session (more heap, more code); rendering progress only on the device screen saves ~nothing (the endpoint costs no heap) and loses the browser UX. **Recommendation: keep polling.** Trim available: drop the `ticks`/`hasPending` diagnostic fields (they exist to debug the now-understood WDT reboot) and the `s_tickCount` global.
7. **Memory anatomy of the download path** (heap unless noted; exact figures for #116 to measure):
   - `ghota` task stack: 16 KB, sized by feel. High-water mark unknown — measure; TLS and write buffers are heap so plausibly 4–6 KB used, but mbedTLS cert verification recursion is the risk case. Candidate savings: several KB.
   - `Update._buffer`: 4 KB, allocated inside `Update.begin()`; the begin-before-TLS ordering (GhUpdate.cpp:80-94) is correct and load-bearing — keep the comment.
   - `BundleClient`: TLS context, handshake scratch, cert-bundle verify — the dominant chunk (~40 KB observed in mission notes) plus mbedTLS I/O buffers (nominally 16 KB in / 4 KB out on IDF defaults — ticket #115 is pinning what the precompiled libs actually bake in and whether any knob is reachable without a custom build).
   - Download write buffer: 4 KB heap (`new (std::nothrow)`) — correctly heap-side; could be merged away only by writing the TLS stream straight into `Update.write` chunks, which is what it already does; no gain available.
   - Strings: the CDN `Location` URL is ~1.2 KB (JWT-bearing — observed); `assetApiUrl`, URL concat temporaries, and the step-1 JsonDocuments disappear with finding 2. Individually trivial, collectively a nice deletion.
8. **Correctness nits** (none currently biting):
   - Race: `tick()` clears `s_hasPending` *before* setting `s_inFlight` (`GhUpdate.cpp:273-275`); a POST landing between the two sees both flags false and can double-queue/overwrite `s_pendingBuf` while the task reads it. Order swap fixes it; deleting the handoff (finding 1) deletes the race. In the direct-spawn shape, guard with a single `s_inFlight` test-and-set in the handler (httpd is single-threaded per request, so a plain bool suffices).
   - Step 2 accepts only 302/301 (`GhUpdate.cpp:152`); GitHub uses 302 today but 307/308 would fail with a confusing message. One-line broadening.
   - `contentLen <= 0` rejects chunked responses — fine for this CDN (sends Content-Length), worth a comment.
   - `s_progress.errorMsg` is written by the download task and read by httpd with no synchronization — worst case a torn/partial message in the UI, never a crash (fixed buffer, always NUL-terminated by `strlcpy`). Acceptable.
   - Mission-notes drift: the CDN host is now `release-assets.githubusercontent.com` (Azure-blob-backed), not `objects.githubusercontent.com` as recorded in map #106/#108 — the cert bundle evidently covers it (device updated successfully), but the notes should be corrected by #118.

## Architecture: is there a better way?

Alternatives weighed:

- **Run the download inside the POST handler (no task at all).** Rejected: PsychicHttp's httpd task has an 8 KB stack (`PsychicHttpServer.cpp:32`) — raising it taxes every request forever — and a blocked httpd means no progress endpoint, no other pages, and the browser's fetch hanging for ~60 s. Kills the UI feedback the user wants to keep.
- **`esp_https_ota`** — pending research ticket #114; would replace findings 2/3/4's hand-rolled plumbing with IDF's, but redirect-header and cert-bundle reachability from precompiled Arduino libs are the open questions.
- **Keep the dedicated Core-0 task, spawn it directly from the POST handler** — recommended baseline regardless of #114's verdict. Same proven runtime shape (Core 0, heap-side TLS, begin-before-TLS), minus the loop() coupling and the mailbox globals. `GhUpdate::tick()` and its `main.cpp` call site go away entirely; `GhUpdate.h` shrinks to `registerRoutes()`.

## Recommended change list (input to decision ticket #117)

1. Spawn `downloadTask` from the POST handler; delete `tick()`, `s_hasPending`, `s_tickCount`, the `main.cpp:74` call, and the mailbox comments. Keep `s_pendingBuf` as the tag's stable storage for the task arg (or a static copy made in the handler).
2. Delete step 1: build `https://github.com/<repo>/releases/download/<tag>/<prefix>-<tag>.bin` from the tag, GET it (no headers needed), take the 302 `Location`, GET the CDN. Removes one TLS handshake, both JsonDocuments, and the fragile pre-`begin()` `Accept` header dance.
3. Remove the two no-op `addHeader("User-Agent", ...)` calls (use `setUserAgent()` if a custom UA is ever actually wanted).
4. Accept 302/301/307/308 in the redirect step.
5. Trim `ghprogress` to `state`/`bytesWritten`/`totalBytes`/`error`; keep 1 Hz polling UI unchanged.
6. Right-size the task stack from #116's high-water measurement (keep a ~4 KB margin over observed).
7. TLS buffer strategy: hold for #115's verdict; adopt only if reachable without a custom-lib pipeline.

Expected effect: peak-heap during OTA drops by roughly one JsonDocument + String set and one handshake's transient churn (the dominant TLS I/O cost is #115's territory); steady-state code shrinks (~60 lines plus a loop() call); the UI stays as-is. Functionality preserved: same endpoints, same UI contract (`/api/update/check` response unchanged; `ghprogress` loses only diagnostic fields the UI never read).

## Empirical checks run (2026-08-21, from workstation)

- `GET api.github.com/.../releases/assets/<id>` without `Accept: application/octet-stream` → **200 JSON** (no redirect); with it → **302** to `release-assets.githubusercontent.com`.
- Same requests with an empty `User-Agent` → **403** from GitHub (both api.github.com endpoints tested).
- `GET github.com/sweetlilmre/smolbase/releases/download/v0.3.1/smolbase-firmware-v0.3.1.bin` (plain, no headers) → **302** to the same CDN.
- arduino-esp32 3.3.11 sources (installed framework): `HTTPClient::addHeader` filters `User-Agent`/`Connection`/`Accept-Encoding` (`HTTPClient.cpp:994`); every `begin()` overload calls `clear()` except `begin(NetworkClient&, String)`; `end()` always clears headers; default UA `ESP32HTTPClient` sent unconditionally (`HTTPClient.cpp:1150`).
