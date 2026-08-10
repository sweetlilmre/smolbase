# Dashboard Asset Pipeline — fonts and condition icons on LovyanGFX

**Ticket:** [#65](https://github.com/sweetlilmre/smolbase/issues/65) (part of map #63)
**Date:** 2026-08-10
**Target:** Reproduce the SmolTV-Pro weather dashboard's type and icons on smolbase's stack: LovyanGFX 1.2.26, ST7789V 240x240, ESP32-D0WD (no PSRAM, 8 MB flash), palette-8 framebuffer default (`SMOLBASE_FB_PALETTE_8`).
**Reference spec:** `D:\source\SmolTV-Pro\scripts\assets.toml` — Teko 400 at lh76 (clock digits, ~96 px render) and lh31 (~49 px), Montserrat 500 lh27/lh31, Inter 800 lh22 (19 glyphs), LVGL stock montserrat 14/28, and 9 basmilius weather icons at 64/60/54 px keyed by OWM codes {1, 2, 3, 4, 9, 10, 11, 13, 50}.

## Recommendation (summary)

**Fonts:** render each face with `lv_font_conv@1.5.3` (npm, pinned) at 4 bpp
with `--format bin`, subset hard (digits+colon for the clock faces), and embed
the `.bin` output as flash byte arrays. LovyanGFX 1.2.26 loads this format
natively at runtime via its (undocumented) `BFFfont` loader — verified in the
pinned library source in this repo's own `.pio/libdeps`. This is the same tool
and the same 4 bpp anti-aliased quality SmolTV-Pro's `assets.toml` metrics
were authored against, at roughly a third of the flash of the VLW alternative.

**Icons:** the nine basmilius (Meteocons, MIT) fill-style SVGs, pinned by tag,
rasterized with resvg and quantized to 4-bit indexed (15 colors + transparent)
exactly as SmolTV-Pro already does, drawn with LovyanGFX's palette
`pushImage(..., transparent, depth, palette)` overload.

**Budget:** ≈ 60–90 KB flash for all custom faces, ≈ 17 KB for all nine icons
— call it **~100 KB of an 8 MB part**; load-time RAM under ~2 KB (glyph index
tables), no per-frame heap growth (glyph bitmaps are read in place from
memory-mapped flash).

**Licenses:** Teko, Montserrat, Inter are all OFL-1.1 (verified upstream);
basmilius weather-icons is MIT (verified). All four need entries in
`docs/THIRD-PARTY.md` as bundled assets; OFL notably requires shipping the
license text and forbids selling the fonts standalone — embedding subsets in
firmware is expressly permitted.

## The decisive finding: LovyanGFX 1.2.26 loads LVGL binary fonts natively

The pinned LovyanGFX copy in `.pio/libdeps/smolbase/LovyanGFX` (v1.2.26, the exact
version smolbase links) contains a runtime font loader for LVGL's binary font
format, alongside the older VLW loader:

- `IFont::font_type_t` enumerates `ft_vlw`, `ft_u8g2`, `ft_lvgl`, `ft_gfx`, … (`src/lgfx/v1/lgfx_fonts.hpp:21-32`).
- `LGFXBase::loadFont(const uint8_t* array, IFont::font_type_t)` accepts a flash
  byte array; `font_type_t::ft_lvgl` instantiates `BFFfont`, anything else
  `VLWfont` (`src/lgfx/v1/LGFXBase.cpp:2545-2610`).
- `BFFfont::loadFont` parses little-endian records tagged `head`/`cmap`/`loca`/`glyf`
  (plus `kern`), with the source comment "head payload layout follows
  font_spec.md" — LVGL's `lv_font_conv` binary format spec. It accepts
  `bits_per_pixel` 1–4 and `compression_algorithm` 0 (raw), 1 and 2 (RLE)
  (`src/lgfx/v1/lgfx_fonts.cpp:1039-1138,1606-1616`).

This means smolbase can use **the same font tool SmolTV-Pro's pipeline already
uses** (`assets.toml`: "'size' is the px size handed to lv_font_conv"), at the
same 4 bpp anti-aliased quality, with RLE compression — no Processing/VLW
detour and no LVGL library dependency at runtime.

### Runtime cost of each font path (verified in the pinned source)

Both runtime formats read glyph data **in place from the memory-mapped flash
array** — `loadFont(array)` wraps the pointer in a `DataWrapper`
(`LGFXBase.cpp:2545-2548`); nothing copies the font to RAM wholesale.

| | VLW (`ft_vlw`) | LVGL bin (`ft_lvgl` / BFF) |
|---|---|---|
| Glyph bitmaps | 8 bpp alpha, uncompressed | 1–4 bpp alpha, optional RLE |
| Load-time RAM | 9 B/glyph index tables, heap (`lgfx_fonts.cpp:1843-1853`) | cmap payload + 4 B/glyph loca table, heap (`lgfx_fonts.cpp:1140-1244`) |
| Per-draw scratch | `alloca(w*h)` on the stack (`lgfx_fonts.cpp:1935`) — ~2.5 KB for a 96 px digit | decompressed glyph on the heap, freed per glyph (`lgfx_fonts.cpp:1706-1742`) |
| Flash for same glyphs | ~2x the 4 bpp size, no compression | baseline |

The VLW path's per-glyph `alloca` is worth avoiding at 96 px on the default
8 KB Arduino loop-task stack; the BFF path uses transient heap instead.

### Anti-aliasing on the palette-8 framebuffer

Text drawn into `Display::frame()` (8-bpp palette sprite) takes the
palette/unreadable branch of the glyph renderer (`lgfx_fonts.cpp:1975`): AA
coverage is blended **between the text color and the sprite's `baseColor`**,
then quantized to the nearest palette entry — it does not read back and blend
against existing pixels (that alpha-blend branch is reserved for readable
non-palette targets, `lgfx_fonts.cpp:2028-2085`). Consequences:

- Call `frame().setBaseColor(<bg>)` (or use `setTextColor(fg, bg)` fill mode)
  so AA fringes blend toward the actual pixels behind the text. On the
  dashboard's flat panels this is exact.
- With the default RGB332 palette, blended fringe colors quantize to 3-3-2
  channels. For large glyphs this is fine; if banding shows, dedicate a few
  palette entries to fg→bg ramp shades via `setPaletteColor`.
- Zero-coverage pixels are skipped (transparent) when not in fill mode, so
  glyphs composite over art without a background box.

Drawing direct to the panel (FB_NONE mode) hits the same branch because the
ST7789 is configured write-only (`readable = false` in `src/core/Display.cpp`),
so the same `setBaseColor` rule applies.

## Fonts: sizes and tooling

### Candidate routes compared

| Route | Tooling | AA | Verdict |
|---|---|---|---|
| **lv_font_conv `--format bin`** | npm `lv_font_conv@1.5.3` (latest, 2024-06-10) — [repo](https://github.com/lvgl/lv_font_conv) | 4 bpp + RLE | **Chosen** — loads via `loadFont(array, ft_lvgl)`; smallest flash; range subsetting built in (`-r 0x30-0x3A`, `--symbols "0123456789:"`) |
| lv_font_conv `--format lvgl` (C array) | same tool | 4 bpp | Rejected — that output is an `lv_font_t` for the LVGL runtime; LovyanGFX's C-array `LVGLfont` support was removed in the 1.2.21 emergency release ("removed all lv_font related files", [releases](https://github.com/lovyan03/LovyanGFX/releases)); only the *bin* loader remains (added via "Sync from M5GFX: add lvgl font rendering support", commit 74b76a9f, 2026-04) |
| VLW (Processing smooth font) | Processing `Create_font.pde` (GUI, not scriptable) or [jdlr-au/vlwconv](https://github.com/jdlr-au/vlwconv) (Python+freetype-py, MIT, **not on PyPI** — clone) | 8 bpp raw | Fallback — works (`loadFont(array)` default `ft_vlw`), but ~2–3x the flash and per-draw `alloca(w*h)` stack use |
| u8g2 fonts (`bdfconv`) | u8g2 tools | 1 bpp | Rejected — no anti-aliasing; unusable for 96 px digits |
| Adafruit GFX fonts | fontconvert | 1 bpp | Rejected — same; the current `FreeSansBold24pt7b` in `src/app/` shows the quality ceiling |
| SmolTV-Pro's `lv_font_gen.py` | Python, freetype-py, byte-identical to lv_font_conv 1.5.3 for `--format lvgl --no-compress` | 4 bpp | Not directly reusable — it deliberately implements only the C writer ("bin/dump writers" are explicitly not implemented). Porting it to emit BFF bin is a later option to drop the Node dependency |

`lv_font_conv` has no self-owned rasterizer — it drives FreeType
(`FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT`), which is
why SmolTV-Pro could re-implement it byte-identically in Python
(`SmolTV-Pro/scripts/lv_font_gen.py` header). The pixels smolbase gets are the
pixels the SmolTV-Pro dashboard shipped.

### Flash estimates (4 bpp; RLE typically saves a further ~25–40%)

Raw 4 bpp glyph cost ≈ `bbox_w × bbox_h / 2`; table overhead is small.
Cross-checked against LVGL's builtin montserrat C fonts (~28/41/106 KB binary
at 22/28/48 px *including* FontAwesome symbols) and against VLW math (8 bpp ≈
2x these numbers):

| Face (assets.toml name) | Spec | Glyphs | 4 bpp raw | est. shipped (RLE) |
|---|---|---|---|---|
| TIME76 — Teko 400, ~96 px render | digits+colon | 11 | ~14 KB | **~9–12 KB** |
| LEAD31 — Teko 400, ~49 px render | digits+colon | 11 | ~4 KB | **~3 KB** |
| WDAY25 — Teko 400 | letters+space | 53 | ~5 KB | ~4 KB |
| BODY27 — Montserrat 500 lh27 | 0x20–0x7E, 0xA0–0x17F (320) | 320 | ~38 KB | **~25 KB** (trim to Latin-1 → ~15 KB) |
| TEMP52 — Montserrat ~48 px (replaces `lv_font_montserrat_48` builtin) | ASCII+° (97) | 97 | ~40 KB | ~27 KB (digits+°+letters subset → ~8 KB) |
| SMALL16 / badge faces (replace montserrat 14/24/28 builtins) | ASCII+° | 97 ea | ~4–12 KB ea | ~3–8 KB ea |
| DATE22 — Inter 800 lh22 | 19 glyphs | 19 | ~2 KB | **~1.5 KB** |

Important: SmolTV-Pro got montserrat 14/24/28/48 **for free from LVGL's
binary** (its `assets.toml` notes rendering them would add ~153 KB). Smolbase
has no LVGL, so every face the dashboard uses must be generated — that is the
one structural difference from the reference pipeline, and aggressive
subsetting (the dashboard's badges draw a known character set) is how the
budget stays near **60–90 KB total**.

### RAM and multi-face use

- Loading a BFF font costs heap for the cmap payload + 4 B/glyph loca table
  (`lgfx_fonts.cpp:1140-1244`) — under ~2 KB for every face above combined.
  Glyph bitmaps decompress per draw into a transient heap buffer (freed the
  same call, `lgfx_fonts.cpp:1706-1742`).
- `loadFont()` holds **one** runtime font per LGFX target. The dashboard needs
  several faces at once: keep one `lgfx::BFFfont` instance per face, each fed
  a `lgfx::PointerWrapper` (public API, `misc/DataWrapper.hpp:155`) over its
  flash array, and switch with `setFont(&face)` — same mechanism `loadFont`
  uses internally, minus the load/unload churn.

## Icons: basmilius weather-icons → indexed bitmaps

### What SmolTV-Pro does (proven at these exact sizes)

`SmolTV-Pro/scripts/build_assets.py` fetches the nine `fill`-style SVGs from
`raw.githubusercontent.com/basmilius/weather-icons/dev/production/fill/svg/{icon}.svg`
(the repo has since been renamed **basmilius/meteocons** — the old name still
redirects; the license is fetched from the new name). Pinning caveat verified
upstream: the repo's `main` branch has been restructured (Figma-driven npm
packages) and no longer carries that directory layout — **pin the `v2.0.0`
tag**, which contains both `production/fill/svg/{name}.svg` (the named files
`assets.toml` references) and, conveniently, `production/fill/openweathermap/`
with icons already named `01d.svg`…`50n.svg` per OWM code. SmolTV-Pro then
rasterizes with
**resvg** (the `resvg-cli` PyPI package drops the binary into the venv — a
pure-Python rasterizer can't handle these icons' gradients/filters, per the
pipeline's own comment), then Pillow-quantizes to a 15-color MEDIANCUT palette
+ 1 transparent entry → 4-bit indexed, ~1.5–2.1 KB per icon, **~16.7 KB for
all nine** (sum of the `data_size` fields in `assets.toml`).

### The LovyanGFX equivalent

LovyanGFX draws palettized source data directly — no LVGL image descriptor
needed:

- `pushImage(x, y, w, h, const void* data, uint32_t transparent, color_depth_t depth, const T* palette)`
  (`src/lgfx/v1/LGFXBase.hpp:422-433`) accepts 4-bit indexed data plus a
  16-entry palette array and a transparent index; the same overloads exist on
  `LGFX_Sprite`, so icons composite into `Display::frame()` with index-keyed
  transparency.
- On the palette-8 framebuffer each icon pixel is converted through the
  sprite's color converter to the nearest RGB332 entry at blit time. The
  basmilius fill icons are flat-ish color art; RGB332 quantization is visually
  acceptable there, and if a hero icon needs exact color, its 15 palette
  entries can be copied into spare slots of the frame palette
  (`Display::frame().setPaletteColor`) — the emitter can reserve that option.
- Anti-aliased icon edges are baked against a chosen matte color at build time
  (quantize over the panel background), because index-keyed transparency is
  binary. That is exactly what SmolTV-Pro's INDEXED_4BIT path already does.
- LovyanGFX also bundles a PNG decoder (`drawPng`, pngle —
  `src/lgfx/utility/lgfx_pngle.c`), so icons *could* ship as PNGs in LittleFS
  and be decoded at draw time; rejected as the primary route — per-draw CPU
  cost plus heap churn on a no-PSRAM chip, versus flat arrays read straight
  from memory-mapped flash.

Per-icon flash at 4-bit indexed: `w*h/2 + 16*2 (RGB565 palette) + ~64` B —
64x64 ≈ 2.1 KB, 60x60 ≈ 1.8 KB, 54x54 ≈ 1.5 KB. **All nine ≈ 17 KB.**

## Licenses / THIRD-PARTY.md

All verified against the upstream license files, 2026-08-10:

| Asset | License | Source of truth |
|---|---|---|
| Teko | OFL-1.1 | [google/fonts `ofl/teko/OFL.txt`](https://github.com/google/fonts/tree/main/ofl/teko) |
| Montserrat | OFL-1.1 | [JulietaUla/Montserrat `OFL.txt`](https://github.com/JulietaUla/Montserrat/blob/master/OFL.txt) |
| Inter | OFL-1.1 | [rsms/inter `LICENSE.txt`](https://github.com/rsms/inter/blob/master/LICENSE.txt) |
| basmilius weather-icons (Meteocons) | MIT | [basmilius/weather-icons](https://github.com/basmilius/weather-icons) (GitHub license API: `MIT`) |

What `docs/THIRD-PARTY.md` needs (new **Bundled assets** entries, following the
existing zones.json pattern):

- One entry per font family: OFL-1.1 notice with each family's copyright line
  (taken from the fetched `OFL.txt`, which the pipeline should also land in a
  `LICENSES/` dir as SmolTV-Pro does). OFL permits embedding/subsetting in
  firmware; the only obligations that bite are shipping the license text and
  not selling the fonts standalone. Reserved Font Name clauses are untriggered
  — we rename nothing, we only subset and rasterize.
- One MIT entry for Meteocons (copyright Bas Milius) covering the nine
  rasterized artworks.
- No new entry for the font *tooling* (lv_font_conv, resvg, Pillow) — build
  tools are not distributed in the firmware or repo, matching how the
  platform/framework section already treats build-time dependencies.

Note the ticket's mention of "LVGL stock montserrat" carries **no LVGL license
obligation** for smolbase: we never ship LVGL code or its tables — we
regenerate those sizes from OFL Montserrat directly.

## Recommended pipeline

One Python script, `scripts/build_assets.py`, mirroring the SmolTV-Pro
pipeline's shape (fetch / plan / render / emit, hash-locked inputs, spec in a
frozen `scripts/assets.toml`) — the repo precedent for scripted assets is
`scripts/pack_fs.py`. Runtime: `uv` venv (per global convention), plus Node
only for `lv_font_conv`.

1. **fetch** — download pinned inputs and record SHA-256s in
   `scripts/assets.lock.toml`; a changed upstream hash is a hard error:
   - Variable TTFs from the canonical Google Fonts repo
     (`raw.githubusercontent.com/google/fonts/main/ofl/{teko,montserrat,inter}/...`)
     plus each family's `OFL.txt`.
   - The 9 SVGs from the `basmilius/weather-icons` **`v2.0.0` tag**,
     `production/fill/svg/{clear-day,partly-cloudy-day,cloudy,overcast,drizzle,rain,thunderstorms,snow,fog}.svg`
     (the exact artworks `assets.toml` maps to OWM codes 1/2/3/4/9/10/11/13/50),
     plus the MIT `LICENSE`. Do not fetch from `main`/`dev` — the branch
     layout changed after the Meteocons rename.
2. **instance** — `fonttools varLib.instancer` pins each variable font to its
   weight (Teko 400, Montserrat 500, Inter 800). Mandatory: variable fonts
   default to their thinnest axis value (SmolTV-Pro learned this the hard way).
3. **render fonts** — `npx --yes lv_font_conv@1.5.3 --font X.ttf --size N
   --bpp 4 -r <ranges> --format bin -o face.bin`. Subset hard: digits+colon
   only for the two clock faces. (Keep default RLE compression; the BFF loader
   handles algorithms 0/1/2. `--no-compress` is the fallback switch if a
   glyph ever renders wrong.)
4. **render icons** — `resvg` (via the `resvg-cli` PyPI package) SVG→PNG at
   64/60/54 px, Pillow MEDIANCUT quantize to 15 colors + transparent index 0,
   pack 2 px/byte.
5. **emit** — write `src/app/weather/AssetsGen.h` (or split font/image
   headers): `constexpr uint8_t FONT_X[] = {...};` for each `.bin`, and per
   icon a 4-bit data array + 16-entry RGB565 palette + a descriptor struct.
   Fonts load with
   `gfx.loadFont(FONT_X, lgfx::IFont::font_type_t::ft_lvgl);`
   icons draw with the palette `pushImage` overload.

Why this exact shape:
- **lv_font_conv over VLW/Processing:** half the flash (4 bpp + RLE vs 8 bpp
  raw), scriptable CLI (Processing's VLW export is GUI-bound), the identical
  tool/spec SmolTV-Pro's `assets.toml` metrics were authored against — the
  `line_height`/`base_line` numbers transfer verbatim — and LovyanGFX 1.2.26
  loads its bin output natively (see above). u8g2 fonts rejected: 1 bpp only,
  no anti-aliasing at 96 px. Adafruit GFX fonts rejected: 1 bpp, and the
  repo's current `FreeSansBold24pt7b` usage shows the ceiling.
- **Only one loaded font per LGFX_Device/Sprite at a time** (`loadFont`
  replaces the previous runtime font). The weather screen needs several faces
  concurrently, so keep one `BFFfont` (or `lgfx::VLWfont`-style) instance per
  face alive instead: construct `lgfx::BFFfont` objects directly, `loadFont()`
  each once at app start against a `PointerWrapper`, and pass them via
  `gfx.setFont(&face)` — the same pattern LovyanGFX uses internally
  (`_runtime_font` is just an `IFont*`). Total load-time heap for all faces at
  ~4–6 B/glyph across ~160 glyphs is under 2 KB.
- **No hand-converted blobs:** everything regenerates from
  `scripts/assets.toml` + lock file; generated headers carry a "do not edit"
  banner, matching `pack_fs.py`'s deterministic-output ethos.

## Residual risks

- The BFF/`ft_lvgl` loader is **undocumented** upstream (synced from M5GFX in
  2026-04, present in the 1.2.26 tag and in our pinned copy). First
  implementation step should be a smoke test: generate one face
  (`TIME76`-equivalent, digits+colon), `loadFont(..., ft_lvgl)`, render
  "12:34" to the framebuffer on-device. If it misbehaves, the fallback is
  mechanical: same pipeline, `vlwconv` (or `--no-compress` first), ~2x flash.
- The per-draw heap allocation for BFF glyph decompression (~2–3 KB transient
  for a 96 px digit) happens under `tick()`; on this no-PSRAM chip watch for
  fragmentation alongside TLS buffers — the clock redraws once a second, so
  pressure is low, but it belongs in the app's soak test.
- `lv_font_conv` needs Node on the dev machine (`npx --yes` pins the version;
  no global install). If that dependency grates, port SmolTV-Pro's
  `lv_font_gen.py` to emit the bin format — its verifier already proves pixel
  parity with lv_font_conv 1.5.3.
