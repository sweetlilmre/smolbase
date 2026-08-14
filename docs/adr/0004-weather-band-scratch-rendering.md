# 0004 — Weather screen renders via an app-owned 16-bpp band scratch, not the core framebuffer

**Status**: accepted (2026-08-14)

## Context

The weather screen originally drew direct to the panel with per-band dirty
flags: every repaint cleared its band to black on the live panel and redrew —
visible flicker, worst on the clock — and the marquee needed its own 13.4 KB
heap sprite, surrendered to every TLS handshake and restored after (the
#74/#94 RAM choreography).

Meanwhile the core allocates a 57.6 KB static (.bss) 8-bpp framebuffer in
every build — placed static precisely so the heap stays contiguous for TLS —
and the weatherclock build, the only one doing TLS, never used it (#102).

Composing into that framebuffer was tried in two steps, and both taught
something worth keeping on record:

1. **Full-frame composition at 30 Hz** (the BoingScreen pattern). Failed
   twice over. The palette (raw-index) framebuffer garbles every *computed*
   color — anti-aliased BFF glyph blends and pushImage's RGB565→dst icon
   conversion write values that are not palette indices (blue sun, fringed
   glyphs) — which forced a true-color RGB332 framebuffer mode into the core.
   And the frame budget doesn't close: a full-frame present is ~23 ms of SPI
   at 40 MHz (the panel only accepts 16-bit color on this interface, so wire
   cost is 2 B/px regardless of buffer depth), plus 15–25 ms re-rasterizing
   anti-aliased typography every pass. Boing survives the same 23 ms present
   because its compose is ~5 ms: the ball is pre-rendered, its animation is
   14 palette pokes. AA type is the most expensive thing this chip rasterizes.
   Result on-device: slow, jittery marquee and full-frame tearing.

2. **Banded composition into the persistent framebuffer** with a partial
   `Display::present(y, h)`. Worked — flicker gone, marquee back to 30 px/s,
   ~2 ms band pushes — but exposed the waste: the painters clear-and-redraw
   their own bands anyway, so the 57.6 KB of persistence does almost nothing
   (its only real uses were the seconds label's full-width row push and the
   one full present at screen entry). The app only ever composes one band at
   a time.

## Decision

The weatherclock env builds with `SMOLBASE_FRAMEBUFFER=0`: the core
framebuffer compiles out of this build entirely. The weather screen owns a
single **16-bpp (RGB565) scratch sprite, 240 px wide and one clock band
(64 rows, 30.7 KB) tall** — the tallest indivisible band, since Teko-96
glyph ink is 60 px. Every band painter composes into the scratch at y = 0
and pushes it to the panel at the band's real y (`pushSprite` — stock
LovyanGFX, no core API involved; this is exactly the "partial-frame 16-bpp
sprite" escape hatch the core Display docs prescribe).

Band cadences: marquee strip at 30 Hz (time-based scroll, 1 px per 33 ms);
**the whole clock band — hh:mm, seconds, or the identity overlay — redraws
every second** (a few ms of rasterization and a ~6 ms push at 1 Hz is under
1% duty, and it deletes the seconds-inside-the-clock special case, the
`lastSec = -1` force-repaint dance, and the overlay erase logic); date daily;
top and gauge bands per fetch or settings change. Bands taller than the
scratch (the 72-row top band) compose in two clipped passes — rasterizing
twice per fetch is free.

The scratch is **static (.bss)**, for the same TLS-contiguity reason the core
buffer was.

## Consequences

- Net RAM: 57.6 KB static freed, 30.7 KB taken — **~27 KB returned** to the
  build's headroom, on the firmware with a ~49 KB TLS heap peak. The marquee
  sprite and its fetch-window choreography stay gone (the #94 hook seam
  remains in WeatherData, unused, documenting the fetch window).
- **Full RGB565 color**: the RGB332 quantization visible in round one (amber,
  the weekday blue) disappears, AA glyphs blend at full fidelity, and pushes
  need no pixel conversion — the scratch is already in wire format.
- No flicker by construction: clears happen in the scratch; a band lands on
  the panel in one short write. Short pushes also shrink the tearing window
  ~10× versus full-frame presents.
- The core's RGB332 framebuffer mode and `Display::present(y, h)` (added for
  step 2) keep no consumer in this repo. They stay — guarded, documented,
  zero cost when unselected — but a future architecture pass should know the
  weather app tried them and moved past them; do not re-propose full-frame
  or core-buffer composition for typography-heavy screens (steps 1–2 above
  are the evidence).
- Shrinking the scratch further (e.g. 36 rows, −13.4 KB) is possible by
  glyph-splitting the clock into two clipped passes; the halves land ~6 ms
  apart, a transient mid-glyph seam once per second. Untested; treat as a
  one-constant experiment if RAM pressure ever demands it.
- The panel's 12-bit (RGB444) mode was considered for bandwidth and rejected:
  LovyanGFX's ST77xx driver doesn't implement it, banding already reduced
  steady-state bus duty to ~6%, and it would not have rescued the full-frame
  design (compose, not transfer, was the binding cost).
