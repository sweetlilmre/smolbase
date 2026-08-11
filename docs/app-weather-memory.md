# Weather app memory situation

Status: working but tight, as flown on `fw 0.3.0-weather` (map #63, flight
log on ticket #74). This documents what the RAM budget actually looks like
on-device, how the TLS handshake fits into it, and what is still unexplained.
The follow-up investigation is [ticket #77](https://github.com/sweetlilmre/smolbase/issues/77).

## The headline numbers (measured on-device)

| Measurement | Value |
| --- | --- |
| Free heap, stock (boing) firmware, steady state | ~110 KB |
| Free heap, weather firmware, steady state | ~49–57 KB |
| Largest free block at fetch time (with marquee surrendered) | ~57 KB |
| TLS handshake peak consumption | ~49 KB |
| All-time heap minimum during a handshake | **208 B** (pre-fix), ~2.2–3 KB (current) |
| Fetch task stack high-water | ~5 KB used of 10 KB |

The 208-byte trough is the number to remember: before the mitigations, a
TLS handshake consumed essentially every free byte, and the intermittent
`X509 - Allocation of memory failed` / "vrfy callback failed" errors were
OOM wearing a certificate costume. Chain-of-trust debugging (bundle ages,
root transitions) was a red herring for everything except the genuinely
missing default-bundle attach.

## What the handshake needs

- 2 × ~16.7 KB mbedTLS record buffers (in/out) — **contiguous each**,
  compiled into the pinned core, no sketch-level knob.
- ~8–12 KB handshake state: server chain parse (3–4 certs), ECDH, hashes.
- Bundle-verify allocations (parsing the matched root from our embedded
  Mozilla bundle) — small but they arrive at peak pressure, which is why
  they are the first to die.

Rule of thumb from the flight: **≥55 KB free with a ≥34 KB largest block at
handshake start** is the practical gate. `/api/debug/weather` reports
`heapFree` / `heapLargest` / `heapMinEver` for exactly this check.

## What the weather app holds (steady state)

| Consumer | Cost | Notes |
| --- | --- | --- |
| Marquee sprite (672×20, 8 bpp) | 13.4 KB | **Surrendered during every fetch** (`suspendMarquee`) and re-created after — this handoff is what made HTTPS reliable |
| Fetch task stack | 10 KB | high-water says ~5 KB used; conservatively sized |
| BFF font tables (6 faces) | ~3–4 KB | cmap payload + 4 B/glyph loca; glyph bitmaps decode per-draw (transient heap) |
| CA bundle | 0 RAM | ~67 KB of flash, memory-mapped |
| Icons/fonts data | 0 RAM | flash `.rodata` |

## The unexplained gap (~25 KB)

Accounting for the table above (~27 KB) against the stock-vs-weather delta
(~55–60 KB) leaves roughly **25 KB unattributed**. Candidates not yet ruled
out: lwIP/WiFi buffer growth after first TLS use, mbedTLS/esp-tls static
state initialized on first handshake, PsychicHttp per-connection growth from
the debug endpoints, heap fragmentation overhead being miscounted as usage.
This is the core question for the follow-up ticket — measure, don't guess:
the flight's probes (per-stage largest-block snapshots, `heapMinEver`,
stack high-water) took minutes to add and settled arguments the theory
never would have.

## Levers not yet pulled

- Marquee sprite at 4 bpp with a fixed palette (−6.7 KB) — needs care: the
  palette text-render path had color quirks on the screenshot sprite.
- Fetch task stack 10 K → 8 K (−2 KB) once more full-handshake high-water
  data exists.
- Font subsetting: WX_CITY22 carries full Latin-1 (~190 glyphs); ASCII-only
  would shrink its tables.
- A pre-reserved TLS arena, or serializing the screenshot endpoint against
  fetches (today they can theoretically collide; the screenshot's 28.8 KB
  sprite + a handshake peak would OOM).
- Revisit when the platform pin moves: a newer core may allow
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER`, which would dissolve the 33 KB
  record-buffer floor entirely.
