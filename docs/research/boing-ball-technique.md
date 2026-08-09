# How the Amiga Boing Ball Worked, and How to Replicate It on the PALETTE_8 Framebuffer

Research for ticket #45. Primary sources: the Harry Sintonen-commented disassembly of the
original Boing demo and Jimmy Maher's staged C reconstruction (both hosted on the companion
site of *The Future Was Here*), the Amiga Graphics Archive analysis, and the LovyanGFX 1.2.x
source vendored in this repo (`.pio/libdeps/smolbase/LovyanGFX/src/`). Every claim below is
cited; anything not directly confirmed is marked UNVERIFIED.

**TL;DR** — The original Boing ball (Dale Luck & R.J. Mical, first shown at CES January 1984)
never rotates as pixels. The ball is drawn once at startup (with floating-point trig, already
tilted); each checker square is really 7 vertical facet stripes, each painted with its own
color register, using 14 cycling registers total (indices 2–15). Each frame the red/white
assignment of those 14 registers shifts by one — an apparent rotation of 180°/56 ≈ 3.2° per
step — with one "reddish-white" blend register at the leading edge as fake motion blur.
Bouncing is pure hardware scroll (viewport X/Y offset); the grid lives in a 5th bitplane that
doesn't scroll; the shadow is a color-1 region that darkens grey to dark grey and the purple
grid to dark purple. Nothing on screen is redrawn per frame. On smolbase we replicate this
with a pre-rendered 8-bpp palette-index ball sprite whose 14 reserved indices are rotated via
`setPaletteColor()` each frame, blitted with the index-transparent `pushSprite` overload
(verified: palette-8→palette-8 pushes copy **raw indices**, no color conversion). Proposed
palette budget: reserve indices 0x01–0x12 (18 entries: 14 ball + purple + dark purple + two
greys), keeping 238 of 256 RGB332-identity entries; the sacrificed codes are the r=0
dark-blue/teal corner of the RGB332 cube. A 120-px ball with 8×8 visible checkers on a 16-px
purple grid reads as "Boing" on the 240×240 panel; a full procedural sphere render per frame
would cost ~14 ms of trig and is not worth it.

---

## 1. How the original Boing rotation worked

### 1.1 The ball is a static image; rotation is 14 cycling color registers

The Amiga Graphics Archive states it flatly: "nothing on screen is redrawn during the demo";
the ball "is rendered at the beginning of the demo and then 'animated' with the color cycling
method" (https://amiga.lychesis.net/applications/AmigaBoingBall.html,
https://amiga.lychesis.net/specials/ColorCycling.html).

The Sintonen-commented disassembly of the original demo
(http://amiga.filfre.net/misc/Chapter2/boing.asm, linked from
http://amiga.filfre.net/?page_id=5) shows exactly how. `_init_globe` builds the sphere's
facet table with two nested loops:

- **Latitude**: outer loop `d4` runs 8→0 (9 latitude circles), angle = `d4 × 32767/8`
  where the sine table treats 0x10000 as a full circle — i.e. **8 latitude bands of 22.5°
  each**, pole to pole [boing.asm, `_init_globe`, the `moveq #8,d4` loop and the
  `moveq #8,d1 / ldivt / _Sine16` angle computation].
- **Longitude**: inner loop `d5` runs 55→0 (56 steps), angle = `d5 × 32767/56` — **56
  longitude facets across the drawn 180° hemisphere**, each **180°/56 ≈ 3.214° wide**
  [boing.asm, `moveq #$37,d5` and the `moveq #$38,d1 / ldivt` divisor].

Each facet's pen number is computed as:

```
color = ((7 * (latitude_band & 1) + longitude_step) mod 14) + 2
```

[boing.asm, `_init_globe`: the `and.l d0,d2` (d4&1), ×7 via shifts/adds, `add.l d5,d2`,
`moveq #14,d1 / lmodt`, `addq.w #2,d0` sequence storing into `(10,a3)`.]

So the ball uses **color registers 2–15 — 14 cycling entries**. Consecutive longitude facets
use consecutive registers (mod 14); at any instant 7 consecutive registers hold white and 7
hold red, so a "checker square" is really **7 adjacent facet stripes** that happen to share a
color — exactly the "each square is made up of 7 vertical stripes, each with its own palette
colour" description in the Amiga Graphics Archive color-cycling page. Odd latitude bands are
offset by 7 registers, producing the checkerboard.

### 1.2 What one cycle step means

The main loop increments or decrements the cycle position `d4` each frame (direction chosen
by the sign of the horizontal velocity `_vx`), wraps it in 0–13, then rewrites the color
table: 7 entries get `$0FFF` (white), 7 get `$0F00` (red), and **one entry at the leading
edge gets a blend value — fake motion blur** [boing.asm `.L124`–`.L84`; the same logic in C
is boing3.c's `color_cycle` block, where the blend is the "reddish-white strip" `$FDD`].
Maher's reconstruction palette:

```c
USHORT Colors[16]=
{0xAAA,0x666,0xF00,0xF00,0xFDD,0xFFF,0xFFF,0xFFF,0xFFF,0xFFF,0xFFF,0xF00,0xF00,0xF00,0xF00,0xF00};
```

[http://amiga.filfre.net/misc/Chapter2/boing3.c]

One register-rotation step shifts the pattern by exactly one facet stripe =
**180°/56 ≈ 3.21° of apparent rotation per frame**. A full red+white period (one checker
pair, 45°) takes 14 steps. Stepped once per vertical blank (`_WaitTOF` in boing.asm; NTSC
60 Hz), that's ≈193°/s — the familiar lazy spin.

### 1.3 Movement, tilt, and the "wobble"

- **Bounce**: "the horizontal and vertical bouncing motion is accomplished entirely through
  manipulating the X and Y offsets of the viewport" [boing3.c header comment; implemented as
  `Screen->ViewPort.RasInfo->RxOffset/RyOffset` in boing3.c, and the `vp_RasInfo` pokes in
  boing.asm ~line 2077–2100]. Zero pixels move in memory; the hardware scrolls the bitplanes.
  Vertical speed is stepped ×1/×2/×3/×4 by height band to fake gravity [boing3.c
  `adjusted_y_scroll`].
- **Tilt**: the tilt is **baked into the static ball image**. The original drew the ball
  once at startup "using a series of complex floating point trigonometry functions"
  (`_draw_globe` in boing.asm uses the FFP `_SPSin/_SPCos` and degree→radian constants);
  Maher's reconstruction simply stores the same image pre-tilted as bitplane data (a 144×100
  px, 4-plane `struct Image`) [boing3.c comment above `DrawImage`; `{0,0,144,100,4,...}`].
  The exact tilt angle is not stated in either source — UNVERIFIED (visually ~15–20°).
- **Wobble**: the original demo's per-frame work is *only* palette pokes + physics + scroll
  offsets [boing.asm main loop; boing3.c main loop]. **There is no per-frame wobble/tilt
  animation** — the tilt is constant. (Wobbling boing balls come from later remakes.) The
  perceived "wobble" is the palette-cycled spin around a tilted axis.

### 1.4 Screen layout, grid, shadow, and why the palette is "mostly duplicates"

Screen: **320×200 low-res NTSC, 5 bitplanes = 32 colors**
[https://amiga.lychesis.net/applications/AmigaBoingBall.html; boing3.c opens 320×200 and the
asm installs a 5th plane]. Register map, from the `SetRGB4` calls in boing.asm (~line
1713–1740) and the cycle loop:

| Register(s) | Value | Meaning |
|---|---|---|
| 0 | `$AAA` | background grey |
| 1 | `$666` | shadow (dark grey on the background) |
| 2–15 | `$F00`/`$FFF`/blend | the 14 cycling ball entries |
| 16 | `$A0A` | grid purple (grid plane set, nothing else) |
| 17 | `$606` | dark purple — grid line inside the shadow |
| 18–31 | copies of 2–15 | ball in front of the grid plane |

The cycle loop writes every ball color to register *n* **and** *n*+16 [boing.asm `.L113`,
`.L110` etc.: each poke is duplicated with `moveq #$10,d2 / add.l d2,d1`] — that is the
"clever selection of color registers ... why the color palette contains mostly duplicates"
[lychesis]: the 5th (grid) bitplane adds 16 to the color index, so mirroring entries 18–31
makes the grid invisible behind the ball, while indices 16/17 give a purple grid that
darkens correctly under the shadow. The grid plane's pointer is managed separately from the
scrolling ball planes so the background stays put ("this way the bg doesn't move along",
boing.asm comment ~line 2089). Note: lychesis says "3 bitplanes ball, 1 grid, 1 shadow", but
ball indices 2–15 need 4 planes; the asm's register usage (2–15 + mirrors at +16) is
authoritative here. The shadow region reads as color 1/17, i.e. it lives in the low plane(s)
offset from the ball — exact plane assignment of the shadow: UNVERIFIED in detail.

Exact colors (4-bit Amiga RGB → 8-bit): grey `$AAA` = `#AAAAAA`, dark grey `$666` =
`#666666`, red `$F00` = `#FF0000`, white `$FFF` = `#FFFFFF`, blur blend `$FDD` = `#FFDDDD`
(reconstruction value), purple `$A0A` = `#AA00AA`, dark purple `$606` = `#660066`
[boing3.c `Colors[]`; boing.asm `SetRGB4` calls and `$0FFF/$0F00` pokes].

## 2. Replication on smolbase: pre-rendered ball + palette cycling

smolbase's framebuffer is exactly the right substrate. `Display.cpp` builds a static
240×240 8-bpp palette sprite:

```cpp
uint8_t fbData[240 * 240]; // static, .bss — heap stays contiguous for TLS
lgfx::LGFX_Sprite fbSprite(&panel);
...
fbSprite.setColorDepth(8);
fbSprite.setBuffer(fbData, 240, 240, 8);
fbSprite.createPalette();
for (int i = 0; i < 256; ++i) { ... }   // RGB332 identity palette
```

[src/core/Display.cpp:70–106], with `present()` = `fbSprite.pushSprite(0, 0);`
[src/core/Display.cpp:114], and the documented palette contract: "The default palette maps
index i as RGB332 (bits RRRGGGBB) ... Palette changes take effect on the next present()"
[src/core/Display.h:20–24].

**Plan**: render the tilted checkered sphere once at startup into a ball-sized 8-bpp palette
`LGFX_Sprite` (index 0 = transparent surround, facet stripes painted with reserved indices
using the exact original formula `((lat&1)*7 + lon) % 14`), then each frame: repaint
background+grid+shadow into `frame()`, `ball.pushSprite(&Display::frame(), x, y, 0)`, rotate
the 14 reserved palette entries with `frame().setPaletteColor(...)`
[`setPaletteColor(size_t, uint8_t, uint8_t, uint8_t)`, LGFX_Sprite.hpp:303–306], and
`present()`. Cycling cost is 14 three-byte palette writes — effectively free — and takes
effect at the next present, since the palette→RGB565 lookup happens during the push to the
panel (palette source → RGB565 panel goes through `copy_palette_affine`
[pixelcopy.hpp:136–147]).

### How many indices does the ball need?

Derived from the original: 14 cycling entries (7 white + 7 red, one of which is overwritten
by the leading-edge blend each frame — the blend does **not** need a 15th entry). Optional
extra shading (the original had none — flat red/white) would multiply this; skip it.

### Proposed palette budget

Reserve **18 contiguous indices, 0x01–0x12**:

| Index | Role | Color |
|---|---|---|
| 0x01–0x0E | 14-entry ball cycle | `#FF0000` / `#FFFFFF` / `#FFDDDD` blend, rotated per frame |
| 0x0F | grid purple | `#AA00AA` |
| 0x10 | grid-in-shadow | `#660066` |
| 0x11 | shadow grey | `#666666` |
| 0x12 | background grey | `#AAAAAA` |

Remaining: **238 entries keep RGB332 identity**, so `lgfx::color332(r,g,b)` and all existing
text/UI drawing keep working [contract in src/core/Display.h:20–23]. What gets sacrificed:
RGB332 codes 0x01–0x12 all have r=0 (top 3 bits clear) — they decode to the
dark-blue/green/teal corner of the RGB332 cube, e.g. 0x01=(0,0,85), 0x03=(0,0,255) (the
purest blue RGB332 has), 0x0E=(0,109,170), 0x12=(0,146,170). So the UI impact is: **pure/dark blues and dark teals drawn via `color332` will render as ball/grid
colors**. Index 0x00 (black), all greys ≥ index 0x24ish, and every color with any red
component are untouched. If a screen needs true blue while the Boing screen exists, use
0x17/0x1B/0x1F (r=0,g=1 blues, unreserved). Since palette edits are global to the shared
`frame()`, the Boing screen should restore the RGB332 identity entries in its `onExit()`.

One repo-specific caveat found while verifying color plumbing: on a `palette_8bit` sprite,
passing a `uint16_t` color (e.g. `TFT_RED` = 0xF800) goes through `convert_rgb565`, which for
a palette destination resolves to `convert_uint32_to_palette8(c) { return c & 0xFF; }`
[colortype.hpp:861–866, 774–779, 49] — i.e. `TFT_RED` becomes index 0x00 (black). Always use
raw indices / `color332()` on the framebuffer, as Display.h already instructs.

## 3. Movement: blitting an 8-bpp sprite into the 8-bpp framebuffer

`pushSprite` overloads on `LGFX_Sprite` [LGFX_Sprite.hpp:334–338]:

```cpp
template<typename T> void pushSprite(LovyanGFX* dst, int32_t x, int32_t y, const T& transp)
  { push_sprite(dst, x, y, _write_conv.convert(transp) & _write_conv.colormask); }
void pushSprite(LovyanGFX* dst, int32_t x, int32_t y) { push_sprite(dst, x, y); }
```

`push_sprite` builds a `pixelcopy_t` and calls `dst->pushImage(...)`
[LGFX_Sprite.hpp:421–425], which clips and hands off to `Panel_Sprite::writeImage`
[LGFXBase.cpp:1425–1442 → LGFX_Sprite.cpp:433].

### Index-vs-color semantics (verified in source)

**Palette-8 → palette-8 pushes copy raw palette indices; no color conversion and no
palette remapping happens.** Chain of evidence:

1. `pixelcopy_t` constructor: for a palette destination with an 8-bit palette source it
   selects `fp_copy = copy_rgb_affine<rgb332_t, rgb332_t>` and sets
   `no_convert = (src_depth == dst_depth)` [pixelcopy.cpp:40–46].
2. `copy_rgb_affine<TDst,TSrc>` does `d[index].set(color_convert<TDst,TSrc>(raw))`, and the
   same-type `color_convert` primary template is the identity `{ return c; }`
   [pixelcopy.hpp:249–268; colortype.hpp:529]. Bytes (indices) are copied verbatim; the
   pixel's final color is whatever the **destination** palette says.
3. The `transp` argument is likewise an **index**: on a palette-8 sprite
   `_write_conv.convert()` resolves to `convert_uint32_to_palette8(c) = c & 0xFF`
   [colortype.hpp:774–779, 49], and the copy/skip loops compare it against the raw source
   byte (`raw == param->transp`) [pixelcopy.hpp:261; skip_rgb_affine likewise].

Consequence: the ball sprite must be authored in the *frame's* index space (reserved indices
0x01–0x0E, transparent = 0x00), which is exactly what we want.

### Fast path vs per-pixel path

`Panel_Sprite::writeImage` [LGFX_Sprite.cpp:433–491] takes a row-`memcpy` fast path only if
**all** of: destination rotation is 0, `transp == NON_TRANSP` (no transparent overload
used), `no_convert` (identical color depth), and the buffer allows memcpy
(`use_memcpy()` is false only for PSRAM buffers [SpriteBuffer.hpp:79]; `fbData` is internal
.bss). Any transparent-color push therefore runs the per-pixel `fp_copy`/`fp_skip` loop
[LGFX_Sprite.cpp:481–490].

Cost at 240 MHz (ESP32-S3): a 120×120 ball push with transparency touches 14,400 source
pixels; the index-copy inner loop is a few loads/stores plus a compare per pixel
(~15–25 cycles) → **≈1–1.5 ms/frame**. The non-transparent memcpy path on the same area
would be tens of microseconds, but transparency is required for a round ball, and 1 ms fits
easily: the frame budget is dominated by `present()` — 240×240×16 bpp = 921,600 bits at
`SMOLBASE_SPI_HZ` 40 MHz [include/smolbase_config.h:21; src/core/Display.cpp:23] ≈ **23 ms
of SPI time (~43 fps ceiling)** plus the CPU-side palette→565 conversion feeding it.

## 4. Alternative considered: procedural sphere per frame

Rendering the checkered sphere mathematically each frame means, per ball pixel
(~π·60² ≈ 11,300 px): normalize to sphere coords, apply tilt rotation, compute
longitude/latitude (`atan2`, `asin`), add the spin phase, and pick red/white. With hardware
FPU that's roughly 200–400 cycles/px → **≈10–19 ms/frame at 240 MHz**, on top of the 23 ms
present — frame rate roughly halves. The standard optimization — precompute the per-pixel
(lat, lon) mapping once into a lookup table — *is* the pre-rendered-index-bitmap approach:
the LUT entry ((lat&1)*7+lon)%14 is exactly the palette index. Conclusion: **procedural
per-frame rendering is not sane here; the Amiga's own trick (pre-render once, cycle 14
palette entries) costs ~14 palette writes + one 1-ms blit per frame and is the right
answer.** (This is also historically faithful: the original's startup delay *was* the
one-time trig render [boing3.c comment; boing.asm `_draw_globe`].)

## 5. Grid + shadow rendering, and whether partial redraw pays

Cheapest faithful composition, painted back-to-front into `frame()` each frame:

1. `fillScreen(0x12)` — background grey (57.6 KB memset in internal SRAM, ~0.1–0.3 ms).
2. Grid: vertical/horizontal `drawFastVLine/HLine` in index 0x0F. ~30 lines ≈ tens of µs.
3. Shadow: the original look is "shadow behind the grid": grey→dark grey, purple
   grid lines→dark purple inside the shadow ellipse [boing.asm registers 1 and 17]. Two
   cheap options: (a) draw the shadow *before* the grid as a filled ellipse in 0x11, then
   draw grid lines with a per-span index choice (0x0F outside / 0x10 inside the ellipse);
   or (b) simpler and near-identical on 240 px: draw shadow ellipse 0x11, then grid on top
   in 0x0F everywhere — loses the darkened-grid-in-shadow nuance. Option (a) recommended;
   it's just an x-range test per grid line. Alternatively pre-render a second "shadow
   stamp" sprite (ball silhouette, all-0x11 + 0x10 where grid lines cross is overkill —
   grid moves never, shadow moves every frame, so compute at blit time).
4. Ball: `ball.pushSprite(&frame(), x, y, 0)`.
5. Rotate palette entries 0x01–0x0E; `present()`.

**Partial redraw does not pay.** `present()` pushes the full 240×240 frame to the panel
unconditionally [src/core/Display.cpp:114], so panel bandwidth (~23 ms) is constant no matter
how little of the sprite changed. The only thing dirty-rect logic could save is internal-RAM
writes: full clear + grid ≈ 57.6 KB + small, versus restoring ~2 ball-sized regions
(~29 KB) — a difference of well under half a millisecond against a 23 ms frame. Keep the
dumb full repaint; it's simpler and its cost is noise. (An Amiga-faithful "never repaint"
scheme — keep the ball fixed in the framebuffer and fake motion — has no hardware-scroll
equivalent here and would gain nothing for the same reason.)

## 6. Size, proportions, and colors for a 240×240 square panel

Original proportions: 320×200 screen, ball image 144×100 px [boing3.c `struct Image Ball` =
`{0,0,144,100,4,...}`] — i.e. 45% of screen width, 50% of height; the 144:100 pixel ratio is
round-ish on NTSC low-res's non-square (taller-than-wide) pixels, whereas our ST7789 pixels
are square [panel 240×240, src/core/Display.cpp:40–43].

Proposal for smolbase:

- **Ball diameter 120 px** (50% of frame, matching the original's visual weight; leaves
  120 px of travel horizontally and ~100 px of bounce vertically).
- **Checkers**: 8 latitude bands × 8 visible longitude tiles (22.5° both ways), which is
  what the original's math produces [Section 1.1]; build it as 56 longitude facet stripes
  × 8 latitude bands with the original index formula so the 14-entry cycle works unchanged.
  Tilt the whole texture mapping by ~17° when pre-rendering (exact original angle
  UNVERIFIED; tune by eye against boing_orig.mp4,
  http://amiga.filfre.net/wp-content/uploads/2021/03/boing_orig.mp4).
- **Rotation speed**: one cycle step per frame at ~40 fps ≈ 128°/s (original: 3.21° × 60 Hz
  ≈ 193°/s); step every frame and let it be slightly statelier, or advance the cycle by
  wall-clock time (3.21° per 16.7 ms) for fidelity. Reverse cycle direction when horizontal
  velocity flips, as the original does [boing.asm `.L124`; boing3.c].
- **Grid**: 16-px spacing (15 cells across), 1-px lines, purple `#AA00AA` on grey
  `#AAAAAA`; optionally the original's perspective skirt (bottom few rows fanning outward
  and compressing — visible in boing_orig.mp4) if cheap. Original exact spacing:
  UNVERIFIED (the asm's bg render loop runs 16 iterations, consistent with a 16-cell wall).
- **Shadow**: ball-sized ellipse offset right (original offsets right and slightly down;
  exact offset UNVERIFIED — ~+15%, tune by eye), dark grey `#666666`, grid lines within it
  dark purple `#660066`.
- **Colors** (from Section 1.4): red `#FF0000`, white `#FFFFFF`, blur blend `#FFDDDD`,
  grey `#AAAAAA`, dark grey `#666666`, purple `#AA00AA`, dark purple `#660066`.

## Sources

Web (Amiga originals):

- http://amiga.filfre.net/?page_id=5 — *The Future Was Here* (Jimmy Maher, MIT Press)
  Chapter 2 companion: original demo archive, Sintonen-commented disassembly, staged
  reconstruction, videos.
- http://amiga.filfre.net/misc/Chapter2/boing.asm — original Boing demo disassembly,
  comments by Harry Sintonen (`_init_globe` facet/color-register math; `SetRGB4` palette
  setup; the `.L124`ff color-cycle loop; `_WaitTOF`; RasInfo scroll pokes).
- http://amiga.filfre.net/misc/Chapter2/boing3.c — Maher's reconstruction stage 3
  (16-color palette array incl. `$FDD` blend; 144×100 ball image; `color_cycle` logic;
  viewport-offset bounce; comment on the original's startup-time FP trig render).
- https://amiga.lychesis.net/applications/AmigaBoingBall.html — Amiga Graphics Archive
  analysis (nothing redrawn; color cycling; hardware scrolling; 32 colors "mostly
  duplicates"; shadow/grid register trick).
- https://amiga.lychesis.net/specials/ColorCycling.html — color-cycling technique page
  (7 stripes per square, 7 white + 7 red palette entries).
- https://www.generationamiga.com/2020/04/14/amiga-history-the-story-of-the-boing-ball/ —
  history: written by R.J. Mical and Dale Luck at/for CES January 1984 (secondary source).
- http://amiga.filfre.net/wp-content/uploads/2021/03/boing_orig.mp4 — video of the original
  demo (visual reference).

LovyanGFX 1.2.x source (D:\source\smolbase\.pio\libdeps\smolbase\LovyanGFX\src\lgfx\v1\):

- LGFX_Sprite.hpp:303–306 (`setPaletteColor`), 334–338 (`pushSprite` overloads incl.
  transparent), 398–417 (`create_palette`), 421–425 (`push_sprite` → `pixelcopy_t` →
  `dst->pushImage`).
- LGFX_Sprite.cpp:433–491 (`Panel_Sprite::writeImage`: memcpy fast-path conditions at :436,
  per-pixel `fp_copy`/`fp_skip` loop at :481–490).
- misc/pixelcopy.cpp:27–46 (`pixelcopy_t` ctor: palette-8→palette-8 selects
  `copy_rgb_affine<rgb332_t,rgb332_t>`; `no_convert = src_depth == dst_depth`).
- misc/pixelcopy.hpp:136–147 (`copy_palette_affine` selection for palette→RGB panel pushes),
  249–268 (`copy_rgb_affine`: raw copy + `raw == transp` compare).
- misc/colortype.hpp:49 & 774–779 (`convert_uint32_to_palette8(c) = c & 0xFF` — transp and
  draw colors on palette sprites are raw indices), 529 (same-type `color_convert` identity),
  861–866 (`convert()` dispatch by argument type).
- misc/SpriteBuffer.hpp:79 (`use_memcpy()` false only for PSRAM buffers).
- LGFXBase.cpp:1425–1442 (`pushImage` clipping → `_panel->writeImage`).

smolbase (D:\source\smolbase\.claude\worktrees\agent-ac0c4cda2e99f6d23\):

- src/core/Display.cpp:70–71 (static 240×240 8-bpp buffer), 93–107 (PALETTE_8 setup, RGB332
  identity palette loop), 114 (`present()` = full-frame `pushSprite(0,0)`), 23 & 40–43
  (SPI freq, 240×240 panel).
- src/core/Display.h:12–27 (framebuffer contract: RGB332 default palette, `color332()`
  index mapping, palette changes take effect on next `present()`).
- include/smolbase_config.h:21 (`SMOLBASE_SPI_HZ` 40 MHz), 27–29 (PALETTE_8 selected).
