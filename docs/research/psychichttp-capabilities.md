# PsychicHttp capabilities and version choice

Resolves wayfinder ticket 0003. Researched 2026-08-08 against primary sources:
the [hoeken/PsychicHttp](https://github.com/hoeken/PsychicHttp) repo (README,
`src/`, `examples/`, issues/PRs), the PlatformIO registry API
(`api.registry.platformio.org/v3/packages/hoeken/library/PsychicHttp`), and the
ESP-IDF v5.5 `esp_http_server` header/docs.

## TL;DR — version pin recommendation

**Pin `hoeken/PsychicHttp @ 3.1.2` (exact pin in `platformio.ini`:
`lib_deps = hoeken/PsychicHttp@3.1.2`).**

The ticket's "v1 vs v2" framing is out of date: the project has moved to a v3
line. Registry history (PlatformIO registry API):

| Line | Latest | Released | Status |
|------|--------|----------|--------|
| v1   | 1.2.1  | 2024-08-14 | Legacy, no releases since Aug 2024 |
| v2   | 2.2.0  | 2026-05-03 | Superseded by v3; brought middleware, unlimited endpoints, regex URIs, URL rewrite (2.0.0), arduino-esp32 3.3 compat (2.1.0), memory-leak + path-traversal fixes (2.2.0) |
| v3   | 3.1.2  | 2026-06-24 | Current. 3.0.0 added native ESP-IDF (no Arduino component needed); 3.1.1 fixed memory-exhaustion / added WebSocket backpressure; 3.1.2 fixed ArduinoJson v6 builds |

Rationale:

- **v1 is effectively unmaintained** (last release Aug 2024) and predates the
  v2/v3 fixes (memory leaks, path traversal, LittleFS file-handling fix in
  2.1.2, `esp_netif` compatibility in 2.2.0).
- **v3 explicitly targets IDF 5.x**: PR #210 added "idf v5.x CI" and
  component-manager examples; the repo ships `pio-arduino` (pioarduino)
  examples, and pioarduino-specific startup bug #103 is closed/fixed.
- **arduino-esp32 3.x**: 2.1.0's changelog calls out "Arduino ESP32 3.3.0
  compatibility"; v3 inherits it. The Arduino-facing API is continuous from v2.
- 3.1.x had a fast patch cadence (3.1.0 → 3.1.2 in three weeks), so pin the
  exact version for template reproducibility rather than `^3`. All versions
  are on the PlatformIO registry under owner `hoeken`, framework `arduino`,
  platform `espressif32`; sole dependency is ArduinoJson.
- Fallback if v3 misbehaves on the Small TV Pro: `2.2.0` is the most-patched
  pre-v3 release and API-compatible for everything smolbase needs.

## Capability map

### Gzip'd static files from LittleFS — built in

`server.serveStatic("/", LittleFS, "/www/")` serves a directory. Per the
README: "If it finds a file with an extra .gz extension, it will serve it as
gzip encoded" — i.e. request `/app.js`, and if only `/www/app.js.gz` exists it
is served automatically. The header is set in
`src/PsychicFileResponse.cpp`, no manual work needed:

```cpp
if (!download && !fs.exists(spath.c_str()) && fs.exists((spath + ".gz").c_str())) {
    spath += ".gz";
    addHeader("Content-Encoding", "gzip");
}
```

Content-Type is derived from the *original* extension (the `.gz` suffix is
ignored for MIME detection). `PsychicStaticFileHander.cpp` (note the upstream
filename typo) also supports `setCacheControl()` / Last-Modified / ETag with
correct 304 handling for `If-Modified-Since` and `If-None-Match`, and a
default-file (`index.html`) fallback for directory paths. Files above the 8 KB
chunk size are sent as chunked responses (README).

**Plan:** ship the web UI as `.gz`-only files in `/www/` on LittleFS; no
uncompressed copies needed.

### Captive portal — good fit, with an ordering rule

Primary source: `examples/arduino/arduino_captive_portal/` (merged via PR #94,
Aug 2024, explicitly a "Captive portal and OTA update examples for Arduino"
contribution).

- **DNS:** standard arduino-esp32 `DNSServer` coexists fine —
  `dnsServer.start(53, "*", WiFi.softAPIP())` plus
  `dnsServer.processNextRequest()` in `loop()`. PsychicHttp runs in its own
  FreeRTOS task (ESP-IDF httpd), so it doesn't block the loop.
- **Catch-all:** the example subclasses `PsychicWebHandler` with `canHandle()`
  returning `true` and registers it via `server.addHandler()` **last** —
  "handlers are triggered on a first created / first triggered basis", so the
  catch-all must come after real routes or it eats them.
- Alternatively `server.onNotFound(...)` is a first-class API; the README's
  own example uses it with `response->redirect(url.c_str())` to bounce every
  unmatched request — exactly the captive-portal redirect shape (respond to
  OS probe URLs like `/generate_204` with a redirect to the portal IP).
- Wildcard routes exist too (`server.on("/upload/*", ...)`), and request
  filters `ON_AP_FILTER` / `ON_STA_FILTER` let the same server instance serve
  portal routes only in AP mode (filter bug vs Async Webserver was fixed in
  issue #27, Dec 2023).

### Multipart upload + OTA — established pattern

Primary source: `examples/arduino/arduino_ota/arduino_ota.ino` (same PR #94).

- `PsychicUploadHandler` with `onUpload(request, filename, index, data, len,
  last)` chunk callback + `onRequest` completion callback, attached to
  `server.on("/update", HTTP_POST, uploadHandler)`.
- Update.h integration: `Update.begin(UPDATE_SIZE_UNKNOWN, command)` on
  `index == 0`, `Update.write(data, len)` per chunk, `Update.end(true)` on
  `last`; `onRequest` checks `Update.hasError()` and returns 200/500. Filename
  suffix selects `U_FLASH` vs `U_SPIFFS` (usable for LittleFS images too).
- **Gotcha (README):** "multipart requests don't know the total size of the
  file until after it has been fully processed" — hence
  `UPDATE_SIZE_UNKNOWN`; you can't pre-validate the image size against the
  partition.
- **Gotcha (example):** don't call `ESP.restart()` inside the handler —
  the example sets a flag and restarts from `loop()` via a separate
  `/restart` endpoint so the HTTP response gets out first. Also,
  `Update.abort()` replaces the first (real) error with an abort error.

### WebSocket / EventSource — first-class (future extension surface)

Both are built in (README):

- `PsychicWebSocketHandler` with `onOpen` / `onFrame` / `onClose`; mount with
  `server.on("/ws", &websocketHandler)`.
- `PsychicEventSource` (SSE) with `onOpen` / `onClose` and
  `client->send(...)`; to push from outside a callback, store
  `client->socket()` (an int, not the pointer) and look it up with
  `getClient(socket)`.
- No-PSRAM relevant tuning (3.1.x): `-D PSYCHIC_WS_MAX_FRAME_SIZE=2048`,
  `-D PSYCHIC_WS_RX_STATIC_BUFFER` (opt-in static RX buffer "for
  heap-constrained boards", PR #247), and `PSYCHIC_WS_MAX_PENDING_FRAMES`
  (default 8 pending frames/client) as backpressure so a stalled client can't
  exhaust the heap (added 3.1.1).
- Native-IDF builds need `CONFIG_HTTPD_WS_SUPPORT=y`; arduino-esp32 3.x
  already enables it.

### Memory characteristics vs raw `esp_http_server`

PsychicHttp **is** a wrapper over ESP-IDF `esp_http_server` (single server
task, synchronous handler dispatch), so per-connection cost is essentially the
raw httpd cost plus small C++ handler/String overhead — unlike
ESPAsyncWebServer's per-connection AsyncClient objects. `server.config` is the
raw `httpd_config_t`, directly adjustable before `begin()`.

Defaults (ESP-IDF v5.5 `esp_http_server.h` `HTTPD_DEFAULT_CONFIG`):
`stack_size = 4096`, `task_priority = tskIDLE_PRIORITY+5`,
`max_open_sockets = 7` ("3 sockets are reserved for internal working of the
HTTP server"), `max_uri_handlers = 8`, `backlog_conn = 5`, 5 s recv/send
timeouts, `lru_purge_enable = false`.

PsychicHttp v3 overrides (from `src/PsychicHttpServer.cpp`):

- `stack_size = 8192` — comment: "file I/O via VFS/LittleFS needs a deep call
  chain". So budget ~8 KB for the httpd task, double the raw default.
- `uri_match_fn = MATCH_WILDCARD` — "new internal endpoint matching — do not
  change this!!!".
- `max_uri_handlers` is computed automatically at `start()` from registered
  endpoints (v2.0.0's "unlimited endpoints"); no manual sizing needed
  (v1 required you to bump `server.config.max_uri_handlers` yourself).
- Optional async worker mode (`ENABLE_ASYNC`): spawns worker tasks and forces
  `max_open_sockets = ASYNC_WORKER_COUNT + 1` so one socket stays free for
  quick synchronous requests. Leave this **off** for smolbase — each worker
  costs another task stack.

No-PSRAM ESP32 guidance:

- Keep the default `max_open_sockets = 7` (or trim to 4–5). It must fit under
  lwIP's socket budget (`CONFIG_LWIP_MAX_SOCKETS`, 10 by default in IDF)
  alongside DNSServer, NTP, and any consumer sockets.
- Set `server.config.lru_purge_enable = true` for the captive-portal server —
  phones fire many parallel probe connections and LRU purging prevents socket
  starvation (PsychicHttp only enables it by default in async mode).
- **Avoid HTTPS**: README states SSL "can only handle 2 connections at a time.
  Each SSL connection takes about 45k ram" — a non-starter without PSRAM.
- README benchmarks: "ESPAsyncWebserver crashes under heavy load on each
  test" while PsychicHttp stays up; its WebSocket throughput is lower than
  alternatives (~38 rps/connection), fine for a settings UI.

## Gotchas summary

1. Handler registration order matters; register the captive-portal catch-all
   (or rely on `onNotFound`) after all real routes.
2. Multipart uploads have unknown total size until complete — use
   `UPDATE_SIZE_UNKNOWN` and check partition fit at `Update.end()`.
3. Restart after OTA from `loop()`, never inside the handler.
4. Handlers run synchronously in the single httpd task — a slow handler blocks
   all requests; keep handlers short (or accept it for a settings UI).
5. Upstream source file `PsychicStaticFileHander.cpp` has a typo'd name —
   irrelevant to users, confusing when reading source.
6. Don't touch `server.config.uri_match_fn` in v3.
7. PsychicHttp pulls in ArduinoJson; 3.1.2 is the first v3 that compiles
   against ArduinoJson v6 (fixed 2026-06-24). Prefer ArduinoJson v7 anyway.

## Sources

- README: https://github.com/hoeken/PsychicHttp (features, gzip quote, SSL
  memory, benchmarks, WS/SSE API, upload API, onNotFound/redirect example)
- Releases: https://github.com/hoeken/PsychicHttp/releases (version history,
  changelog claims per release)
- PlatformIO registry API:
  https://api.registry.platformio.org/v3/packages/hoeken/library/PsychicHttp
  (published versions, dates, dependency, framework/platform)
- Source: `src/PsychicHttpServer.cpp` (stack_size 8192, wildcard matching,
  auto max_uri_handlers, async socket math), `src/PsychicFileResponse.cpp`
  (Content-Encoding: gzip), `src/PsychicStaticFileHander.cpp` (gz lookup,
  ETag/Last-Modified/304)
- Examples: `examples/arduino/arduino_captive_portal/`,
  `examples/arduino/arduino_ota/` (merged in PR #94)
- Issues/PRs: #94 (captive portal + OTA examples), #27 (ON_AP_FILTER fix),
  #103 (pioarduino HTTPS startup fix), #210 (IDF v5.x CI), #247 (static WS RX
  buffer for heap-constrained boards)
- ESP-IDF v5.5 `esp_http_server.h` HTTPD_DEFAULT_CONFIG and
  https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/protocols/esp_http_server.html
