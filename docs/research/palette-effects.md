# 256-Color Demo Effects on the PALETTE_8 Framebuffer

Research behind the demo screen's effect roster (issue #105) — the four effects added
alongside the Boing ball, why those four, and how each was adapted to a 240×240 8-bpp
framebuffer that costs ~24 ms to push. Sources are the canonical technique write-ups
(Lode Vandevenne's computer graphics tutorial, Sean Barrett-era "oldschool demo effects"
collections, Wikipedia's technique articles) plus the Ferrari/EffectGames material on
color cycling as an art form. Everything measured is marked as measured; everything not
yet confirmed on hardware is marked UNVERIFIED.

**TL;DR** — The 1990s 256-color effect canon splits into two families, and the split is
about *where the animation lives*. The wormhole is the extreme case and the one that took
four attempts: three of them computed geometry at runtime before the original's source
turned up and showed it computed nothing at all. **Palette-animated** effects (color cycling, plasma,
tunnel) precompute a field of indices once and animate by rewriting the palette or by
adding a single offset to the index — the pixels barely change, the colors do all the
work. **Buffer-animated** effects (fire, rotozoomer) recompute the index field each
frame, cheaply enough that it doesn't matter, and use the palette as a mapping function
rather than a clock. The roster deliberately ships two of each, plus Boing, which is the
purest palette animation of the lot (14 registers, zero pixel writes). All four new
effects render the full frame in an estimated 2–3 ms against a ~8 ms budget (24 ms of
every 33 ms frame belongs to `present()`), and the roster's memory is a
single pool sized to whichever effect is running — 14.4 KB at most, and nothing at all
for the two that need none.

---

## 1. Why palette animation was the era's superpower

Color cycling animates by changing the *color table*, not the image: "storing one image
and changing its palette requires less memory and processor power than storing multiple
frames of animation" (https://en.wikipedia.org/wiki/Color_cycling). Mark Ferrari's
*Living Worlds* scenes are the high-water mark of the technique as art — rain, surf,
firelight and day/night cycles, all from one static 8-bit image and a moving palette
(https://www.effectgames.com/effect/article-Old_School_Color_Cycling_with_HTML5.html,
https://www.effectgames.com/demos/canvascycle/). Ferrari's own description is the best
one-line summary of why it reads as motion: it works "in pretty much the same way light
bulbs on a theater marquee do."

That property is worth more here than it was on a 486. On this panel a full-frame
`present()` costs ~24 ms of a 33 ms frame (measured, ticket #46) — the pixel push, not
the pixel *generation*, is the frame-rate ceiling. Any effect that spends its animation
budget in the palette instead of the buffer is essentially free.

The catch, and the reason none of the effects here are *purely* palette-animated: the
clock overlay is redrawn on top of every frame, which destroys the field underneath it.
A static index field has to be re-laid every frame regardless. So the roster uses palette
animation for what it is uniquely good at (motion that costs nothing) and pays a 1–3 ms
re-lay cost it cannot avoid anyway.

## 2. The four effects

### 2.1 Plasma — three fields, one palette clock

The canonical construction: "generate a palette, generate a plasma buffer that contains
the results of each sine function calculation, and then use them to draw each pixel with
the correct color from the palette, then shift the palette every frame"
(https://en.wikipedia.org/wiki/Plasma_effect, https://lodev.org/cgtutor/plasma.html).

`PlasmaEffect` ships three fields, cycled by tap, because the canonical one has a ceiling
that no amount of extra sine terms lifts:

**0 — Classic.** Separable sines of `x`, `y` and `x+y`, each the average of two sines at
unrelated frequencies. Separability is what makes it cheap: the horizontal term is one
240-entry table per frame hoisted out of the pixel loop, and the diagonal `f(x+y)` is the
same table read from a per-row offset. The inner loop is three lookups, two adds, a mask
and a store. **But separable means the field is a fixed lattice.** Change the phases and
it can only slide past itself; it cannot change shape. That is the "a bit static" ceiling,
and it is structural.

**1 — Ripples.** Three moving centres, each throwing a circular wave, summed. Nothing is
separable, so as the centres orbit on unrelated periods the interference genuinely
deforms. Two tricks make circles affordable:

- **A table indexed by r², holding the sine of the root.** No `sqrt` and no multiply per
  pixel (https://www.4rknova.com/blog/2016/11/01/plasma).
- **Squared distances are still separable even though circles are not:**
  `(x-cx)² + (y-cy)²` is one row of squares per axis per source, so a frame costs six
  short table builds rather than a multiply per pixel.

One bug worth recording, because it was visible as jitter on the panel and invisible in
the code: the difference `x - cx` was truncated to an integer *before* squaring. The
centres orbit in float, but the tables only saw whole half-res pixels, so the entire
pattern snapped two screen pixels whenever a centre crossed a boundary — and since C
truncates toward zero, the snap was asymmetric across `d = 0`. Squaring in float and
quantising afterwards keeps the sub-pixel motion, at a cost of 720 multiplies per frame
in the table build and nothing in the inner loop.

**2 — Interfere.** One circular wave *multiplied* by a diagonal one. Multiplying rather
than adding pinches the bands into sharp nodes wherever either term crosses its midpoint,
and the two beat at different rates so the nodes sweep.

Flavours 1 and 2 run at half resolution and expand 2x, which buys back the extra
per-pixel work.

**The ramp runs on its own clock** — four cyclic ramps, thirty seconds each, crossfading
over 1.5 s rather than snapping. It is deliberately independent of the flavour, so the
field and the colours never change together and the same field is seen four ways. Cyclic
matters for the same reason it always does here: the rotation wraps, and a ramp whose
first and last stops differ drags a seam through the field once per cycle. The fire's
ramp is the roster's only non-cyclic one, and it is the only one never rotated.

### 2.2 Fire — the original, not the copy of the copy

The textbook fire is well documented and everyone implements it: seed a random bottom
row, average each cell with the three below it plus one two rows down, subtract a little
so it dies out as it rises (https://seancode.com/demofx/, https://lodev.org/cgtutor/fire.html).
That version was built here first, tuned twice on the panel, and then thrown away — because
the fire everyone is copying is **firedemo** by Javier "Jare" Arevalo of Iguana (1993),
and his does almost none of that.

From his own HTML5 reprise (https://github.com/TheJare/FiredemoHTML5):

- **The blur is all eight neighbours, divided by eight.** It is symmetric: the kernel has
  no idea which way is up.
- **Nothing rises through the kernel.** The buffer is *scrolled* up one row per tick, and
  that alone sets the flame's speed. The textbook version's "heat climbs one row per
  step, so the step rate is the speed" is a property of a design he did not use.
- **The cooling is not random.** A quarter of cells are selected by the low bits of the
  neighbour sum — there is no PRNG anywhere in the effect — and cooled by one, **wrapping
  at zero**. Within four rows of the base, cold cells are cooled too, so `0 - 1 = 255`:
  white-hot. Every spark in the fire is that wrap. Jare has written that the effect came
  out of "a few programming mistakes"; this is the best of them, and it is not something
  anyone arrives at by reasoning about fire.
- **The field is tiny and heavily magnified** — 80×50 in his, 60×60 at 4× here. The
  softness comes from magnifying a small, heavily-blurred field, not from smoothing a
  large one. It also makes this the second-cheapest effect in the roster.
- **The palette has a cyan toe.** The coldest embers are dark blue-green rather than
  black-red, and the ramp tops out at *yellow* and never reaches white — white is left
  for the wrap-around sparks. It is the most recognisable thing about this fire and no
  invented ramp finds it.

The 64-entry palette is copied verbatim — his data, not a technique — and both it and the
algorithm are used under the MIT licence, (C) 2013 by Javier Arevalo. See
[../THIRD-PARTY.md](../THIRD-PARTY.md).

The lesson is the one the wormhole taught in a different key: the canonical description of
a classic effect is often a reconstruction of it, and the original is both stranger and
better. Go to the source when there is one.

### 2.3 Wormhole — the one that was already solved

This effect was built four times. The first three were runtime geometry — a distance-and-
angle field around a screen point, then a ray-cast cylinder, then a ray-cast funnel with a
rim — each one tuned against reference frames and each one wrong in a way the next was
supposed to fix. The fourth was to stop computing anything.

**The original never computed a tunnel.** Psycho Neurosis (Asphyxia, 1994) drew a
640×400 image of palette indices, stored it in four Mode-X planes, loaded it into video
memory once, and then did no per-pixel work at all for the rest of the scene. Everything
that moves is palette animation plus hardware panning. The reconstruction of that scene —
`PART3_TUNNEL.PAS`, in a separate repository — states it in its own header, and reading
it ended the search immediately.

What smolbase runs is that scene, ported:

- **The field is the demo's own image**, unresampled, in flash. Its values are a sawtooth
  of indices 1..225; scaling or interpolating them would invent a ring across the wrap
  that was never there, so nothing touches them.
- **The palette is the demo's own three 225-byte channel tables**, 15 bands of 15, 6-bit
  VGA values. They are not merely colour: printed as a 15×15 grid of set/clear, the red
  table draws a **letter A framed in black**. The tiles on the tunnel wall and the dark
  grid between them are painted by the palette, which is why the field on its own looks
  like plain concentric rings — and why every ramp invented for the earlier attempts was
  doomed regardless of how carefully it was tuned.
- **Two rotations per step, against each other.** `RotateBandsFine` turns each 15-colour
  band left by one, independently; `RotateBandsCoarse` turns the whole 225-entry table
  right by one whole band. One alone shimmers in place; the pair is what makes the rings
  flow inward.
- **The window spirals.** 3° and 0.1 of radius per frame, from the largest radius that
  keeps the window inside the field — 100 there for a 320×200 view, 80 here for a square
  one. Rates are converted 70 → 30 Hz for wall-clock fidelity, and the rotations step
  through an accumulator because 2.33 of them per frame is not something you can do a
  fractional number of times.

One deliberate departure: the original ended the scene when the radius reached 1. A clock
face cannot end, so this one turns around and spirals back out.

**What the three abandoned attempts are worth.** They are recorded because the failure was
not in the tuning, and no amount of it would have helped:

- A field of `(depth + angle)` summed into one byte cannot shift its two coordinates
  independently, so falling and turning become the same motion. Separate planes fix that
  and cost twice the memory.
- Pitching the camera sideways puts the vanishing point out to one side at mid-height and
  fills the frame with concentric rings — still a view straight down the pipe, however far
  the centre is moved. Pitch has to be vertical.
- Pitch stored as a *tangent* cannot reach the useful end of its own range. Near the
  horizon it runs to infinity, so no value of the constant could produce the view being
  asked for. An angle in degrees can.
- Inside a cylinder there is no horizon — perpendicular to the axis is wall — so "looking
  across the lip" is not expressible in that model at any parameter value. That needs a
  surface with a rim.

Every one of those is a case of the *model* being unable to express the target, discovered
only by tuning parameters inside it. The general lesson is cheaper than the specific ones:
when tuning keeps almost working, suspect the model, not the numbers. And when the target
is a fixed image someone already drew, ship the image.

### 2.4 Rotozoomer — one matrix per frame, two adds per pixel

The rotozoomer "maps the coordinate of that pixel backwards through a sin/cos transform
to determine which coordinate of the source image to use", with a scale factor folded in
for zoom and modulo wrapping for tiling (https://seancode.com/demofx/). The affine
mapping is the point: because the transform is linear, the texture coordinate at pixel
`x+1` is the coordinate at `x` plus a constant, so `sin`/`cos` are called **twice per
frame**, never per pixel.

`RotozoomEffect` walks that in 16.16 fixed point with a 64×64 texture, so wrapping is a
mask (`& 63`) rather than a modulo — the same power-of-two requirement Lode's tunnel
notes for tiling. Two textures ship: the XOR fractal (`(x^y)`, one instruction, an
unmistakable scene signature) and a double-sine plaid that tiles seamlessly and turns
the whole screen into moving bands under a rotating palette.

## 3. The palette split, and why 128

Everything above wants "the whole palette", and the clock overlay wants some of it. The
screen splits the 256 entries: **0x00–0x7F to the running effect, 0xF0–0xF5 to the
overlay** (black plus the five user-picked identity colors), the rest left as the
framebuffer's documented RGB332 identity. The overlay sits that high because the wormhole
replays a palette of 225 colours starting at index 1, and anything lower would be
repainted by a tunnel.

128 is not a compromise, it is the useful number: it makes the cycling primitive
`(value + phase) & 0x7F` a single masking instruction per pixel, where a 240-entry bank
would need a modulo or a branch. A 128-entry ramp is also period-correct — VGA's 256
registers were routinely split between an effect and the UI/sprites sharing the screen
with it. The visible gain is at the overlay's end: with its own reserved entries the
clock now shows the **exact** RGB the user picked, where the old screen quantized every
text color through `color332()`.

## 4. Effects considered and not shipped

- **Metaballs / blobs** (additive radial fields through a banded palette) — the strongest
  remaining candidate, and the natural fifth. Left out only to keep this change to four.
- **Copper bars** — pure palette animation, but it is a *raster* trick: it wants a color
  change mid-scanline, which needs either racing the beam or a per-row palette. Neither
  exists behind a framebuffer that is pushed to the panel in one DMA transfer.
- **Starfield / 3D dots** — genuinely 90s, but it animates by moving points, not colors,
  and would have been the one effect in the roster with nothing to say about the palette.
- **Voxel landscape** — the per-column ray walk is affordable, but it needs a height map
  *and* a color map in RAM, and both would have to fit the one shared pool.

## 5. Budget

Per 33 ms frame at the fixed 30 Hz timestep, on the measurements in
[docs/building-your-app.md](../building-your-app.md) (ticket #46):

| Item | Cost |
| --- | --- |
| `present()`, full 240×240 push at 40 MHz SPI | ~24 ms (measured) |
| Identity overlay (two text passes, bitmap fonts) | ~2 ms (measured, as part of the old Boing scene) |
| Effect `step()` — full-frame index field + palette writes | ~2–3 ms (UNVERIFIED estimate: 57,600 pixels at roughly 8–12 cycles each on a 240 MHz core) |
| Half-res effects (fire, two plasma flavours) expand 2x instead | roughly a quarter of the per-pixel cost, plus ~1 ms of expansion |
| Headroom | ~4 ms |

The wormhole is the cheapest of the five despite being the most faithful: its frame is
240 row copies out of flash and 225 palette writes, with no per-pixel arithmetic at all.

One-time cost, paid on switch-in and never again: the Boing ball's pre-render, ~11 K
pixels of `asinf`/`atan2f` — tens of milliseconds, a perceptible but sub-frame pause on a
long press. UNVERIFIED on hardware.

## 5a. Memory, which turned out to be a safety property

An OTA streams a ~1.2 MB image through the web server and needs free heap to do it.
Measured on device: with ~77 KB free an upload succeeds; with ~60 KB it fails at the first
parse, every time — and this board has no serial port to recover through. An earlier cut
of the wormhole held a 61 KB pool and made the device unflashable while it ran.

Two fixes, only one of which worked:

- **Parking on `SysEvent::OtaStarting`** (drop to the calm clock, free the pool) does
  *not* rescue it. The upload dies before the first chunk callback, which is where that
  event is posted. It is still right to stop drawing and allocating during an update, but
  it is not what makes the device flashable.
- **Not holding the memory** does. The pool is sized per effect
  (`Effect::scratchBytes`) rather than to the worst case. Measured free heap on device,
  with the calm clock's 122.4 KB as the baseline:

  | Effect | Free heap | Pool |
  | --- | --- | --- |
  | Calm clock | 122.4 KB | — |
  | Wormhole | 121.5 KB | ~0 (field and palette are in flash) |
  | Plasma | 118.0 KB | 4.4 KB (ring table + source rows) |
  | Fire | 114.9 KB | 7.4 KB (two 60x61 buffers) |
  | Boing ball | 107.6 KB | 14.8 KB (pre-rendered ball) |
  | Rotozoomer | 105.0 KB | 17.4 KB (128x128 texture) |

  The worst case leaves ~105 KB free, well clear of the ~77 KB at which an upload was
  observed to succeed.

The rule worth carrying: an app's memory budget on an OTA-only device is set by what the
updater needs, not by what the app can get away with while running.

## 6. Sources

- Color cycling as a technique and as art: https://en.wikipedia.org/wiki/Color_cycling,
  https://www.effectgames.com/effect/article-Old_School_Color_Cycling_with_HTML5.html,
  https://www.effectgames.com/demos/canvascycle/, https://www.markferrari.com/
- Plasma: https://en.wikipedia.org/wiki/Plasma_effect, https://lodev.org/cgtutor/plasma.html
- Tunnel: https://lodev.org/cgtutor/tunnel.html
- Fire, rotozoomer, and the wider effect canon: https://seancode.com/demofx/
- Boing ball (the roster's first effect): [boing-ball-technique.md](boing-ball-technique.md)
- The wormhole's field, palette and motion: the Psycho Neurosis reconstruction
  (`src/PART3_TUNNEL.PAS`, `src/gen/P3PAL.INC`, `docs/06-part3-scene1-tunnel.md` in that
  project) — a separate repository, not vendored here. Provenance: [../THIRD-PARTY.md](../THIRD-PARTY.md)
- Frame budget and framebuffer contract: [../building-your-app.md](../building-your-app.md),
  `src/core/Display.h`
