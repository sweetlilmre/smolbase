# Research: HTTPS for weather fetches — heap cost and cert strategy

Ticket: #64 (part of the Weather Clock map, #63)
Date: 2026-08-10

## Question

Can the weather-clock App fetch api.openweathermap.org / api.open-meteo.com /
geocoding-api.open-meteo.com over **HTTPS** within smolbase's heap budget on the
Small TV Pro (ESP32, no PSRAM), and if so with what certificate strategy?
SmolTV-Pro deliberately uses plain HTTP to dodge the mbedTLS handshake spike; the
charter says we try HTTPS.

Ground truth throughout: the exact toolchain smolbase pins — pioarduino
`platform-espressif32` 55.03.311, arduino-esp32 **3.3.11**, precompiled IDF libs
**5.5.5** (verified locally in
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig`) —
plus ESP-IDF 5.5 docs, espressif GitHub issues, and live TLS chains.

## Answer in one line

**Yes — one TLS connection at a time fits, budget ~45–50 KB of clean free heap
per fetch, verify with the built-in ESP x509 bundle (the 3.x default), and keep
plain HTTP as a config-selectable fallback.** Measure `/api/status` `heapFree`
on-device before committing.

## 1. TLS client heap cost on arduino-esp32 3.x

Espressif's measured table for the `https_request` example on ESP32 (server
validation, [ESP-IDF v5.5 mbedTLS docs](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/protocols/mbedtls.html)):

| Config | Heap used |
|---|---|
| Default | **42,196 B** |
| SSL Variable Length | 42,120 B |
| Keep Peer Certificate disabled | 38,533 B |
| Dynamic TX/RX buffer | 22,013 B |

Anatomy of the ~42 KB under the Arduino defaults: two static record buffers of
16 KB in + 16 KB out (`MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, symmetric) plus
~5–10 KB of ssl/x509/pk contexts. This is held **for the life of the
connection**, not just the handshake; the handshake adds a transient few KB on
top (peer chain parsing — and the peer cert is retained afterwards, since
`CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y` is baked into the shipped libs).

Field corroboration: HTTPS starts failing with alloc/`-0x2700` errors around
~44 KB free ([arduino-esp32 #2175](https://github.com/espressif/arduino-esp32/issues/2175));
one esp-idf reporter failed at 48,428 B free because **fragmentation** matters —
the two 16 KB buffers each need a large contiguous block
([esp-idf #7387](https://github.com/espressif/esp-idf/issues/7387)). mbedTLS
allocates from internal RAM only
([ESP-FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/mbedtls.html)),
which is all this chip has anyway. A long or RSA-4096 server chain pushes the
peak higher ([arduino-esp32 #2896](https://github.com/espressif/arduino-esp32/issues/2896)).

**Planning numbers: ~45–50 KB free (unfragmented) heap at connect time; ~37–42 KB
held while the socket is open; largest-free-block must stay ≥ ~17 KB.** One
concurrent TLS connection is fine; two is asking for trouble.

### Knobs — what the precompiled core actually allows

The Arduino core ships mbedTLS **precompiled**, so sdkconfig knobs are baked in;
`build_flags` overrides of `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` are ignored
([arduino-esp32 #6286](https://github.com/espressif/arduino-esp32/issues/6286)).
Verified in the shipped 3.3.11 / IDF-5.5.5 sdkconfig:

- `MBEDTLS_SSL_MAX_CONTENT_LEN=16384`; `MBEDTLS_ASYMMETRIC_CONTENT_LEN` and
  `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` **not set**.
- `MBEDTLS_DYNAMIC_BUFFER` effectively **off**: the
  [lib-builder defconfig](https://github.com/espressif/esp32-arduino-lib-builder/blob/master/configs/defconfig.common)
  requests it, but IDF 5.5's Kconfig has `depends on !MBEDTLS_SSL_PROTO_DTLS`
  and DTLS is enabled, so Kconfig silently drops it. The tempting 22 KB
  dynamic-buffer figure is **not attainable** on stock pioarduino builds.
- MFL (`MBEDTLS_SSL_MAX_FRAGMENT_LENGTH`) **is compiled in**
  (unconditional in the shipped `esp_config.h`, line 1215), but it buys nothing:
  `NetworkClientSecure` exposes no pre-handshake hook for
  `mbedtls_ssl_conf_max_frag_len()`, and without
  `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` the 16 KB buffers are allocated at full
  size regardless of the negotiated fragment length.
- `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` **not set** → every connection is TLS 1.2.
  Verified all three target hosts accept TLS 1.2 (`openssl s_client -tls1_2`,
  2026-08-10).

**Bottom line: no sketch-level knob shrinks the ~40 KB footprint.** The
mitigations are architectural: one connection at a time, connect → GET → read →
`stop()` promptly, fetch when heap is cleanest. (Rebuilding the static libs via
esp32-arduino-lib-builder could save ~14–20 KB, but that abandons the pinned
prebuilt core — out of scope for a template.)

## 2. smolbase's headroom (doc-based; measure before committing)

smolbase was already built to leave TLS room:

- The 57.6 KB framebuffer is **static `.bss`**, not heap —
  `src/core/Display.cpp:70`: `uint8_t fbData[240 * 240]; // static, .bss — heap
  stays contiguous for TLS`. It lowers the heap ceiling but never fragments it.
- Palette: one-time 1 KB boot allocation (`Display.cpp:96`). Demo effect
  scratch (the pre-rendered Boing ball and every other effect's working
  buffer): 14.4 KB heap, only while that Screen runs — one shared allocation
  (`src/app/effects/Effect.cpp`; it was `BoingScreen.h:21` at research time).
- PsychicHttp: ~8 KB httpd task plus per-connection buffers
  (`docs/research/psychichttp-capabilities.md` — budget ~8 KB for the task).
- A bare arduino-esp32 sketch with WiFi up typically reports ~180–200 KB free;
  subtracting smolbase's static buffers and server, expect roughly **120–160 KB
  free heap** in steady state — comfortably 2–3× the ~50 KB TLS requirement,
  *if* the largest free block stays healthy.

**Caveat: these are paper numbers.** This research was done off-device (no
flasher; the dev unit is OTA-only). Before committing to HTTPS, flash a build
and read `heapFree` from `/api/status` (`src/core/Web.cpp:131`) with the
weather Screen active; ideally also log
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` around a test fetch. If the
largest free block near fetch time is under ~20 KB, HTTPS is off the table and
the HTTP fallback applies.

## 3. Certificate strategy

`NetworkClientSecure` in the pinned 3.3.11 core (verified locally, matches
[release/v3.x](https://github.com/espressif/arduino-esp32/blob/release/v3.x/libraries/NetworkClientSecure/src/NetworkClientSecure.h))
supports all three modes:

| Mode | API | Flash | Heap | Risk |
|---|---|---|---|---|
| **Built-in ESP x509 bundle** (default in 3.x when no CA is set — `ssl_client.cpp:63` wires `esp_crt_bundle_attach`) | nothing, or explicit `setCACertBundle()` | ~67 KB (measured: `x509_crt_bundle_length` = 68,983 B in the shipped `libmbedtls.a`; linked only if used) | ~nil — blob stays in flash; verify parses only the matching root (~1–2 KB transient) | Near-zero rotation risk (~130+ Mozilla NSS roots, [esp_crt_bundle docs](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/protocols/esp_crt_bundle.html)) |
| Pinned per-host root | `setCACert(pem)` | ~1.3–2 KB per root | ~1–2 KB parsed x509 while client lives | Breaks silently when the provider changes CA (see below) |
| No validation | `setInsecure()` | 0 | saves ~1–2 KB only — record buffers dominate, still ~40 KB | MITM-able; and it does **not** meaningfully reduce heap |

Live chains (openssl s_client, 2026-08-10):

- **api.open-meteo.com** and **geocoding-api.open-meteo.com** (identical):
  leaf ← Let's Encrypt `YR2` ← `ISRG Root YR` (new 2025 root), cross-signed by
  **ISRG Root X1**. Per [letsencrypt.org/certificates](https://letsencrypt.org/certificates/),
  the YE/YR roots (generated 2025-09-03) aren't in trust stores yet, so served
  chains still terminate at ISRG Root X1 (valid to 2035) — pinning X1 works
  today, but LE is mid-transition, so a pin has a multi-year (not decade) shelf
  life. LE intermediates rotate every few years (R3 → R10–R13 → YR1/YR2) —
  never pin an intermediate.
- **api.openweathermap.org**: leaf ← `Sectigo Public Server Authentication CA
  OV R36` ← `Sectigo ... Root R46` (valid to 2046), cross-signed by
  `USERTrust RSA Certification Authority` (valid to 2038). Roots are long-lived,
  but OWM is merely a Sectigo customer and can switch CA at any renewal.

**Verdict: use the built-in bundle.** Heap cost is indistinguishable from
pinning; the only price is ~67 KB flash, and smolbase's 8 MB flash /
`partitions.csv` layout doesn't feel it. Pinning saves flash we don't need to
save and adds a real "weather silently dies in N years" failure mode on a
device meant to be a long-lived template. `setInsecure()` buys no heap and
costs the integrity of the data — reject it.

## 4. Endpoint HTTPS support (verified 2026-08-10)

| Endpoint | HTTPS | TLS 1.2 | Note |
|---|---|---|---|
| `https://api.open-meteo.com/v1/forecast` | 200 OK | yes | HTTPS is the only documented scheme ([open-meteo.com/en/docs](https://open-meteo.com/en/docs)) |
| `https://geocoding-api.open-meteo.com/v1/search` | 200 OK | yes | same infra/chain as api host |
| `https://api.openweathermap.org/data/2.5/weather` | 401 (no key — TLS fine) | yes | OWM serves both HTTP and HTTPS ([FAQ](https://openweathermap.org/faq)) |

## 5. Recommendation

1. **Implement HTTPS** with `NetworkClientSecure` + the default ESP x509 bundle
   (no `setCACert`, no `setInsecure`). One fetch at a time, serially:
   geocode → forecast, each as connect → GET → parse → `stop()`.
2. Stream-parse the JSON (ArduinoJson filter/stream) rather than buffering the
   body — the TLS buffers already cost 33 KB; don't stack a large body buffer
   on top of them.
3. **Gate on measurement**: before the weather App lands, verify on-device that
   free heap ≥ ~60 KB and largest free block ≥ ~20 KB while the fetch runs
   (`/api/status` heapFree + a temporary largest-block log). Paper says this
   passes with 2–3× margin.
4. **Fallback position, stated explicitly**: if the device measurement fails —
   or field devices ever show TLS alloc failures — the fallback is **plain
   HTTP to the same hosts** (OWM serves HTTP; Open-Meteo's documented endpoints
   are HTTPS, so an HTTP fallback there is unsupported territory and would
   need re-verification at that time). Make the scheme a compile-time or
   config flag so the fallback is a switch, not a rewrite. Weather data is not
   secret; the API key in the OWM query string is the only thing HTTP exposes,
   which is an accepted cost in the fallback case — it is exactly what
   SmolTV-Pro does today.

## Key uncertainty flags

- The 42 KB IDF figure is one measured example (TLS 1.2, modest chain); real
  peaks vary with the server's chain size and key type.
- Free-heap headroom here is estimated from code and docs, not measured on the
  Small TV Pro — step 3 above is mandatory before the decision is final.
- The 67 KB bundle size is measured from this exact pioarduino 5.5.5 artifact
  and will drift a few KB as Mozilla's root store changes.
