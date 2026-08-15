# 256-Color Demo Effects on the PALETTE_8 Framebuffer

Research behind the demo screen's effect roster (issue #105) — the four effects added
alongside the Boing ball, why those four, and how each was adapted to a 240×240 8-bpp
framebuffer that costs ~24 ms to push. Sources are the canonical technique write-ups
(Lode Vandevenne's computer graphics tutorial, Sean Barrett-era "oldschool demo effects"
collections, Wikipedia's technique articles) plus the Ferrari/EffectGames material on
color cycling as an art form. Everything measured is marked as measured; everything not
yet confirmed on hardware is marked UNVERIFIED.

**TL;DR** — The 1990s 256-color effect canon splits into two families, and the split is
about *where the animation lives*. **Palette-animated** effects (color cycling, plasma,
tunnel) precompute a field of indices once and animate by rewriting the palette or by
adding a single offset to the index — the pixels barely change, the colors do all the
work. **Buffer-animated** effects (fire, rotozoomer) recompute the index field each
frame, cheaply enough that it doesn't matter, and use the palette as a mapping function
rather than a clock. The roster deliberately ships two of each, plus Boing, which is the
purest palette animation of the lot (14 registers, zero pixel writes). All four new
effects render the full frame in an estimated 2–3 ms against a ~8 ms budget (24 ms of
every 33 ms frame belongs to `present()`), and the whole roster shares one 14.4 KB
buffer — the same allocation the Boing ball already made.

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

### 2.1 Plasma — sine sums, palette rotation

The canonical construction: "generate a palette, generate a plasma buffer that contains
the results of each sine function calculation, and then use them to draw each pixel with
the correct color from the palette, then shift the palette every frame"
(https://en.wikipedia.org/wiki/Plasma_effect). Lode's tutorial gives both the sine sums
(`128 + 128*sin(x/16)` style terms, optionally with a radial
`sin(sqrt((x-w/2)² + (y-h/2)²)/8)` term) and the two animation modes: **palette looping**
— `palette[(plasma[y][x] + paletteShift) % 256]` over a static buffer, "computationally
efficient" — or recomputing the sine fields per frame with time folded in, "more
expensive but allows shape variation" (https://lodev.org/cgtutor/plasma.html).

`PlasmaEffect` does **both**, because on this hardware it can afford to. Three fields
(horizontal, vertical, diagonal), each the average of two sines at unrelated
frequencies, drift at their own rates *and* the palette rotates on top. Two adaptations
matter:

- **Separability is what makes it cheap.** A horizontal term depends only on `x` and a
  vertical only on `y`, so each is one 240-entry table computed per frame and hoisted out
  of the pixel loop. The diagonal term `f(x+y)` looks non-separable, but for a fixed row
  it is just the same table read from an offset — a pointer add per row. The inner loop
  is three lookups, two adds, a mask, a store.
- **The ramps must be cyclic.** Lode's tutorial is explicit that plasma palettes must be
  "seamlessly tileable (no discontinuities between last and first colors)", since the
  rotation wraps. Every ramp in this roster therefore repeats its first stop as its last;
  the fire ramp is the sole exception, and it is the one ramp that is never rotated.

### 2.2 Fire — the fuel row and the random bleed

Every version of this effect agrees on the shape: "the bottom row (just offscreen) is
filled with random indices. This becomes the fuel of the fire effect", then each frame
"averages each pixel with adjacent values, then darkens them slightly, causing upward
flame progression" (https://seancode.com/demofx/). The palette runs black → red → orange
→ yellow → white, so the heat number *is* the color and nothing has to decide what a
flame looks like.

`FireEffect` follows that exactly, with the three-tap blur widened by a fourth sample two
rows down (taller flames, less smoke) and a **random** 0–3 bleed-off per cell rather than
a fixed one. The randomness is the entire flicker: a deterministic decay gives a
perfectly still flame that reads as a photograph of fire.

It runs at 120×120 in the shared scratch and doubles up to 240×240 on the way out. That
is not a concession to the ESP32 — mode 13h was 320×200 on a tube that displayed it at
roughly twice that, so chunky is the period-correct look, and here it halves the heat
simulation's cost as a bonus.

### 2.3 Tunnel — the palette wormhole

The reference implementation precomputes two tables and then does nothing but add to
them (https://lodev.org/cgtutor/tunnel.html):

```
distance = ratio * texHeight / sqrt((x - w/2)² + (y - h/2)²)   // ratio = 32.0
angle    = (0.5 * texWidth * atan2(y - h/2, x - w/2)) / π
```

and per frame reads `texture[(distance + shiftX) % texWidth][(angle + shiftY) % texHeight]`,
where the two shifts "independently change the speed of the rotation, and of the moving
forward". The `1/r` distance term is what makes it read as depth rather than as a zoom:
rings crowd together toward the center exactly as a real tunnel's do, so a *constant*
shift per frame looks like constant speed down the pipe.

`TunnelEffect` keeps the geometry and collapses the texture. Two 16-bit coordinate tables
for a 240×240 screen would be 115 KB; there is no such memory on this chip. Instead the
two coordinates are summed into **one byte per pixel** — `(depth + angle) & 0x7F` — over a
120×120 field, and the "texture" becomes the 128-entry palette ramp itself. Consequences,
recorded honestly:

- Forward motion and rotation are no longer independent knobs, because they now share
  one axis. The effect gets its second axis back from the palette instead: the field
  offset advances one way each frame while the ramp rotates the other way, and the eye
  reads the difference as banded light sliding over a moving surface.
- The angle term spans the **full** 128 entries per revolution, so the wrap from 359° to
  0° lands on the same entry it left — no seam. The side effect is a one-armed spiral,
  which is a period-correct look in its own right.
- Lode's tutorial says nothing about the center singularity, where `1/r` outruns the
  pixel grid. Here `r` is clamped to 3 (half-res) units, which confines the aliasing
  shimmer to about a dozen pixels at the vanishing point instead of a quarter of the
  screen. UNVERIFIED how that reads on the panel at 30 Hz.

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
screen splits the 256 entries: **0x00–0x7F to the running effect, 0x80–0x85 to the
overlay** (black plus the five user-picked identity colors), 0x86–0xFF left as the
framebuffer's documented RGB332 identity.

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
  *and* a color map in RAM, and there is one shared 14.4 KB buffer.

## 5. Budget

Per 33 ms frame at the fixed 30 Hz timestep, on the measurements in
[docs/building-your-app.md](../building-your-app.md) (ticket #46):

| Item | Cost |
| --- | --- |
| `present()`, full 240×240 push at 40 MHz SPI | ~24 ms (measured) |
| Identity overlay (two text passes, bitmap fonts) | ~2 ms (measured, as part of the old Boing scene) |
| Effect `step()` — full-frame index field + palette writes | ~2–3 ms (UNVERIFIED estimate: 57,600 pixels at roughly 8–12 cycles each on a 240 MHz core) |
| Headroom | ~4 ms |

One-time costs, paid on switch-in and never again: the tunnel's field build (~14 K pixels
of `sqrtf`/`atan2f`) and the Boing ball's pre-render (~11 K pixels of `asinf`/`atan2f`) —
tens of milliseconds each, i.e. a perceptible but sub-frame pause on a long press.
UNVERIFIED on hardware.

## 6. Sources

- Color cycling as a technique and as art: https://en.wikipedia.org/wiki/Color_cycling,
  https://www.effectgames.com/effect/article-Old_School_Color_Cycling_with_HTML5.html,
  https://www.effectgames.com/demos/canvascycle/, https://www.markferrari.com/
- Plasma: https://en.wikipedia.org/wiki/Plasma_effect, https://lodev.org/cgtutor/plasma.html
- Tunnel: https://lodev.org/cgtutor/tunnel.html
- Fire, rotozoomer, and the wider effect canon: https://seancode.com/demofx/
- Boing ball (the roster's first effect): [boing-ball-technique.md](boing-ball-technique.md)
- Frame budget and framebuffer contract: [../building-your-app.md](../building-your-app.md),
  `src/core/Display.h`
