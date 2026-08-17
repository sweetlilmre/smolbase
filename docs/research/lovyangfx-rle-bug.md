# LovyanGFX BFF RLE decoder — first-pixel guard bug

**Ticket:** [#81](https://github.com/sweetlilmre/smolbase/issues/81)
**Date:** 2026-08-11
**Target:** Explain why `lv_font_conv --bpp 4 --format bin` fonts (default RLE compression,
algorithm ID 1) render double-struck/smeared glyphs through LovyanGFX 1.2.26's `BFFfont`
loader, while `--no-compress` (algorithm ID 0) bins render correctly. Identify the exact
defect line, whether it has been fixed upstream, and the recommended path forward.

> **Resolved upstream (2026-08-17).** The one-line fix below was filed as
> [issue #883](https://github.com/lovyan03/LovyanGFX/issues/883), submitted as
> [PR #884](https://github.com/lovyan03/LovyanGFX/pull/884), merged 2026-08-12 and released
> in **LovyanGFX 1.2.27**. `platformio.ini` is pinned there and
> `patches/lovyangfx-rle-bug.patch` is deleted (#86) — nothing needs re-applying after a
> version bump any more. The pristine 1.2.27 `lgfx_fonts.cpp` is byte-identical to our
> patched 1.2.26 copy apart from one unrelated VLW space-glyph change, which this repo
> does not exercise (the weather app loads BFF bins only). The analysis below is kept as
> the record of how the bug was found.

## Recommendation (summary)

Apply a **one-line local patch** to `decode_rle_bitmap` in the pinned LovyanGFX source.
The fix is trivial, self-contained, and carries no risk; the upstream master has the same
bug and no release newer than 1.2.26 exists as of Aug 2026. Drop `--no-compress` from
`scripts/build_assets.py` once the patch lands. File an upstream issue to route the fix
back to lovyan03.

**Patch** (`src/lgfx/v1/lgfx_fonts.cpp` line 944 in v1.2.26):

```diff
-      if (bs->bit_pos != bpp && prev_v == ret)
+      if (out != 0 && prev_v == ret)
```

## Background: lv_font_conv compression IDs

`lv_font_conv` writes one of three algorithm IDs into the BFF `head` table
(`doc/font_spec.md`, field "Compression algorithm ID"):

| ID | Meaning | lv_font_conv trigger |
|----|---------|----------------------|
| 0 | Raw bits, no compression | `--no-compress`, or `--bpp 1` |
| 1 | RLE + XOR row prefilter (default) | no flags, `--bpp 4` |
| 2 | RLE, no prefilter | `--no-prefilter` |

Source: [`lib/font/table_glyf.js` `getCompressionCode()`](https://github.com/lvgl/lv_font_conv/blob/master/lib/font/table_glyf.js):

```javascript
getCompressionCode() {
    if (this.font.opts.no_compress) return 0;
    if (this.font.opts.bpp === 1)   return 0;
    if (this.font.opts.no_prefilter) return 2;
    return 1;                         // default: 4-bpp with XOR prefilter
}
```

`scripts/build_assets.py` currently passes `--no-compress` (line 71), so all generated
fonts land with ID 0 and the RLE path is never exercised — that is the workaround
installed when the bug was first observed ("first flight (#74) showed LovyanGFX 1.2.26's
BFF RLE decode double-striking glyphs", line 67–69).

### XOR prefilter direction

Before RLE compression, lv_font_conv XORs each pixel row with the row above it
(`lib/utils.js`, `prefilter()` function):

```javascript
return pixels.map((line, l_idx, arr) => {
    if (l_idx === 0) return line.slice();          // first row unchanged
    return line.map((p, idx) => p ^ arr[l_idx - 1][idx]);
});
```

The decompressor must therefore XOR each decoded row with the already-restored previous
row to recover the originals. LovyanGFX does exactly that when `compression_algorithm ==
1` (`lgfx_fonts.cpp:1613–1624` in v1.2.26):

```cpp
if (compression_algorithm == 1)
{
    for (uint32_t y = 1; y < raw_h; ++y)
    {
        uint8_t* row  = &bitmap[y * raw_w];
        uint8_t* prev = row - raw_w;
        for (uint32_t x = 0; x < raw_w; ++x)
            row[x] ^= prev[x];
    }
}
```

The XOR post-filter is correct. The defect is upstream of it, in `decode_rle_bitmap`.

## Root cause: broken first-pixel guard in `decode_rle_bitmap`

### What the guard is supposed to do

The I3BN-style RLE algorithm used by lv_font_conv relies on detecting two
**consecutive** identical pixels to enter run mode. The decoder tracks the previous
pixel in `prev_v` (initialized to 0). Without a guard, if the very first pixel of a
glyph happened to equal `prev_v = 0`, the decoder would incorrectly interpret it as the
second pixel of a run and enter `RLE_REPEATED` state — reading 1-bit RLE control signals
from bits that the encoder wrote as bpp-wide pixel data, misaligning the entire stream.

To prevent this, `decode_rle_bitmap` checks whether the current read is the first pixel:

```cpp
// lgfx_fonts.cpp:939–950 (v1.2.26)
if (state == RLE_SINGLE)
{
    if (bs->bit_pos + bpp > bs->bit_length) break;
    ret = (uint8_t)bs->read_bits(bpp);

    if (bs->bit_pos != bpp && prev_v == ret)   // <-- guard
    {
        count = 0;
        state = RLE_REPEATED;
    }
    prev_v = ret;
}
```

The guard says: "enter repeat-detection only if `bit_pos` has advanced beyond the
position it would occupy after reading just one bpp-wide pixel from the start of the
stream." In a standalone decoder where the bit stream begins at position 0, this is
correct: after the very first pixel, `bit_pos == bpp`, so the condition is false and the
guard suppresses repeat detection.

### Why the guard always misfires

`decode_rle_bitmap` is called from `decodeGlyphBitmap` with a bit stream that does NOT
start at position 0. The caller sets:

```cpp
// lgfx_fonts.cpp:1601–1604 (v1.2.26)
bit_stream_t bs;
bs.data       = glyph_buf;
bs.bit_length = length * 8;
bs.bit_pos    = header_bits;          // non-zero: past the glyph header
```

`header_bits` is the total width of the per-glyph header fields (advanceWidth +
bbox_x/y/w/h in bits, as decoded from the `head` table). For any real font at 4 bpp
this is tens of bits. After `decode_rle_bitmap` reads the first pixel:

```
bs->bit_pos = header_bits + bpp
```

The guard condition becomes:

```
(header_bits + bpp) != bpp   ≡   header_bits != 0   ≡   always TRUE
```

So `bs->bit_pos != bpp` is permanently true regardless of which pixel is being read.
The first-pixel guard never fires. If the first pixel of the compressed glyph bitmap
equals `prev_v = 0` — which it almost always does, because every anti-aliased glyph has
transparent (value 0) left-edge pixels — the decoder enters `RLE_REPEATED` and begins
consuming 1-bit control tokens from data that the encoder wrote as 4-bit pixel values.

The bit stream is misaligned from pixel 2 onwards. The remaining glyph data is decoded
as garbage: opaque ink appears in shifted positions, producing the double-struck /
smeared look reported in #81.

### Why `--no-compress` is unaffected

With `compression_algorithm == 0`, `decodeGlyphBitmap` takes the raw-bits path:

```cpp
// lgfx_fonts.cpp:1606–1609 (v1.2.26)
if (compression_algorithm == 0)
{
    for (uint32_t i = 0; i < pixel_count; ++i)
        bitmap[i] = bs.read_bits(bits_per_pixel);
}
```

`decode_rle_bitmap` is never called; there is no state machine, no guard, no
misalignment. The metrics path (`textWidth`) reads the same header fields regardless of
compression, so advance widths and glyph positions are correct in all cases — consistent
with the observation that layout is right but bitmaps are wrong.

### Why `compression_algorithm == 2` is also affected

The code calls `decode_rle_bitmap` for both IDs 1 and 2, differing only in whether the
XOR post-filter runs. The first-pixel guard fires (or fails to fire) the same way for
both, so ID 2 glyphs would also be misrendered. This is moot for lv_font_conv 1.5.3
output because `--no-prefilter` is never passed in the current pipeline, but any future
attempt to use ID 2 would have the same symptom.

## The fix

Replace the bit-position heuristic with a direct output-count check.
`out` is the number of pixels already written to `dst`; it is 0 only for the very first
pixel, regardless of where the bit stream started.

```diff
--- a/src/lgfx/v1/lgfx_fonts.cpp
+++ b/src/lgfx/v1/lgfx_fonts.cpp
@@ lgfx_fonts.cpp:944 (v1.2.26) @@
-      if (bs->bit_pos != bpp && prev_v == ret)
+      if (out != 0 && prev_v == ret)
```

No other change is needed. The XOR post-filter, the RLE state machine transitions, and
the `RLE_COUNTER` logic are all correct.

## Upstream status

| Question | Finding |
|----------|---------|
| Latest LovyanGFX release | **1.2.26** (most recent as of 2026-08-11) |
| Master branch status | Same `bs->bit_pos != bpp` condition at ~line 1082 (confirmed via fetch of `raw.githubusercontent.com/lovyan03/LovyanGFX/master/src/lgfx/v1/lgfx_fonts.cpp`) |
| BFF support added in | Commit `74b76a9f` ("Sync from M5GFX: add lvgl font rendering support", 2026-04) — present in 1.2.26 |
| Subsequent commits to `lgfx_fonts.cpp` | `f04cce1` "rolled back lvgl font" (2026-04-22), `2e5119f` "fix: environment-dependent build errors" (2026-04-10), `6ecc08e` "lgfx_fonts: restore global font namespace and align with M5GFX" (2026-05-20) — none touch the RLE decoder |
| Upstream issue or PR for this bug | None found (searched open + closed issues for "BFF", "RLE", "lv_font_conv", "compressed font") |

The defect was introduced with the initial BFF implementation and has not been corrected
in any subsequent commit or release.

## Action plan

1. **Patch the pinned source** — apply the one-line diff above to
   `D:\source\smolbase\.pio\libdeps\weatherclock\LovyanGFX\src\lgfx\v1\lgfx_fonts.cpp`
   line 944.

2. **Smoke-test on device** — rebuild with `--no-compress` removed from the
   `lv_font_conv` invocation in `scripts/build_assets.py` (delete line 67–69 comment
   and the `--no-compress` on line 71). Flash and verify a complete render of all font
   faces; RLE compression typically saves 25–40 % flash vs raw.

3. **Remove the workaround** — once the on-device render is clean, remove `--no-compress`
   from `render_font()` and update the comment. This recovers the ~15 KB flash noted in
   the original workaround comment (`build_assets.py:69`).

4. **File an upstream issue** — report to [lovyan03/LovyanGFX](https://github.com/lovyan03/LovyanGFX/issues)
   with the root-cause analysis and the one-line fix. Include a minimal reproducer:
   generate a `.bin` with `lv_font_conv --bpp 4 --format bin` (ID 1), observe smeared
   glyphs; apply the patch, observe clean glyphs.

## Source citations

| Claim | Source |
|-------|--------|
| Compression algorithm IDs 0/1/2 | `doc/font_spec.md` field "Compression algorithm ID", https://github.com/lvgl/lv_font_conv |
| `getCompressionCode()` — default returns 1 | `lib/font/table_glyf.js` (lv_font_conv master) |
| XOR prefilter applies row[y] ^= row[y-1] before compression | `lib/utils.js` `prefilter()` (lv_font_conv master) |
| `decode_rle_bitmap` buggy guard line | `src/lgfx/v1/lgfx_fonts.cpp:944` (LovyanGFX v1.2.26, pinned at `D:\source\smolbase\.pio\libdeps\weatherclock\LovyanGFX`) |
| `bs.bit_pos = header_bits` initialization | `src/lgfx/v1/lgfx_fonts.cpp:1604` (same file) |
| XOR post-filter (correct) | `src/lgfx/v1/lgfx_fonts.cpp:1613–1624` (same file) |
| Raw-bits path (unaffected) | `src/lgfx/v1/lgfx_fonts.cpp:1606–1609` (same file) |
| `--no-compress` workaround + flash cost note | `scripts/build_assets.py:67–71` (`D:\source\smolbase\scripts\build_assets.py`) |
| Master branch has same guard | Confirmed via WebFetch of `raw.githubusercontent.com/lovyan03/LovyanGFX/master/src/lgfx/v1/lgfx_fonts.cpp` (2026-08-11) |
| No upstream fix in any release | GitHub releases page for lovyan03/LovyanGFX; latest = 1.2.26 |
| BFF support introduction commit | Research doc `docs/research/dashboard-asset-pipeline.md` citing commit 74b76a9f |
