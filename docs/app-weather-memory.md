# Weather app memory situation

Status: **comfortable** since the band-scratch rendering rework
([ADR 0004](adr/0004-weather-band-scratch-rendering.md), tickets #102/#103,
2026-08-14). Everything below the "Current numbers" section is the historical
flight record from the tight era (`fw 0.3.0-weather`, map #63, flight log on
ticket #74, follow-up #77) — kept because its measurements and method are
what the rework was built on.

## Current numbers (ADR 0004 era, measured on-device 2026-08-14)

| Measurement | Value | Tight-era value |
| --- | --- | --- |
| Free heap, steady state | **~96 KB** | ~49–57 KB |
| Largest free block, steady state | **~90 KB** | ~29–40 KB |
| All-time heap minimum during a TLS handshake | **~37 KB** | 208 B → ~2–5 KB |
| Boot heap (before WiFi) | ~123 KB | ~95 KB |

What changed (see the ADR for the full arc):

- The 13.4 KB heap **marquee sprite is gone** — the marquee draws into the
  band scratch. The suspend/resume fetch choreography (#74/#94) is retired;
  WeatherData's fetch-window hooks remain in the interface, unused.
- The core's 57.6 KB static framebuffer is **compiled out** of this env
  (`SMOLBASE_FRAMEBUFFER=0`); the app owns a 30.7 KB static 240×64 RGB565
  band scratch instead — net ~27 KB of static RAM returned.
- With a ~37 KB TLS floor and a ~90 KB largest block, the boot-time
  `X509`/OOM fetch transients are structurally gone, and the ≥55 KB-free /
  ≥34 KB-largest-block handshake gate below is cleared roughly 2× over.
- Now-dead levers from the list at the bottom: the 4 bpp marquee sprite
  (no sprite exists) and the screenshot-vs-fetch collision (endpoint removed
  in #82's era). The stack-shrink, font-subsetting, and
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER` levers remain valid if ever needed.

---

# Historical flight record (tight era, fw 0.3.0-weather)

This documents what the RAM budget actually looked like
on-device, how the TLS handshake fits into it, and what was then unexplained.
The follow-up investigation was [ticket #77](https://github.com/sweetlilmre/smolbase/issues/77).

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
