#include "FireEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cstring>

namespace {

// Field size. Jare's was 80x50 magnified to 320x200; ours is square, so 60x60
// magnified 4x fills 240x240 with cells the same size his were. One row of
// slack at the bottom: the render reads a row ahead of the simulation.
constexpr int W = 60;
constexpr int H = 60;
constexpr int HBUF = H + 1;
constexpr int CELLS = W * HBUF;
constexpr int ZOOM = 4;

// Cool even ZERO cells within four rows of the base. That is where sparks come
// from: zero minus one wraps to 255, which the palette shows as white-hot.
constexpr int COOL_FROM = W * (HBUF - 4);
// The bottom rows get a decorative warm-up before display only — it never
// feeds back into the simulation.
constexpr int SMOOTH_FROM = W * (HBUF - 7);

// Javier "Jare" Arevalo's palette, verbatim from FiredemoHTML5: 64 entries of
// 6-bit VGA. Black through a dark
// CYAN toe — the coldest embers are blue-green, not black-red, and that toe is
// the single most recognisable thing about this fire — then deep red, red,
// orange, and out to yellow. It never reaches white. Anything hotter than the
// table is white, which is what the spark wrap lands on.
const uint8_t PAL6[64][3] = {
    {0, 0, 0},   {0, 1, 1},   {0, 4, 5},   {0, 7, 9},   {0, 8, 11},  {0, 9, 12},
    {15, 6, 8},  {25, 4, 4},  {33, 3, 3},  {40, 2, 2},  {48, 2, 2},  {55, 1, 1},
    {63, 0, 0},  {63, 0, 0},  {63, 3, 0},  {63, 7, 0},  {63, 10, 0}, {63, 13, 0},
    {63, 16, 0}, {63, 20, 0}, {63, 23, 0}, {63, 26, 0}, {63, 29, 0}, {63, 33, 0},
    {63, 36, 0}, {63, 39, 0}, {63, 39, 0}, {63, 40, 0}, {63, 40, 0}, {63, 41, 0},
    {63, 42, 0}, {63, 42, 0}, {63, 43, 0}, {63, 44, 0}, {63, 44, 0}, {63, 45, 0},
    {63, 45, 0}, {63, 46, 0}, {63, 47, 0}, {63, 47, 0}, {63, 48, 0}, {63, 49, 0},
    {63, 49, 0}, {63, 50, 0}, {63, 51, 0}, {63, 51, 0}, {63, 52, 0}, {63, 53, 0},
    {63, 53, 0}, {63, 54, 0}, {63, 55, 0}, {63, 55, 0}, {63, 56, 0}, {63, 57, 0},
    {63, 57, 0}, {63, 58, 0}, {63, 58, 0}, {63, 59, 0}, {63, 60, 0}, {63, 60, 0},
    {63, 61, 0}, {63, 62, 0}, {63, 62, 0}, {63, 63, 0},
};
constexpr uint8_t IDX_WHITE = 64; // everything above the table

// His port runs at 60 Hz on requestAnimationFrame; we render at 30, so two
// simulation ticks per frame keeps the flame the speed he tuned it to.
constexpr int TICKS_PER_FRAME = 2;

inline uint8_t dac8(uint8_t v) { return (uint8_t)((v * 255) / 63); }

} // namespace

size_t FireEffect::scratchBytes() const { return 2 * CELLS; }

void FireEffect::enter(lgfx::LGFX_Sprite& f) {
  memset(fx::scratch(), 0, 2 * CELLS);
  for (int i = 0; i < 64; ++i)
    f.setPaletteColor((uint8_t)i, dac8(PAL6[i][0]), dac8(PAL6[i][1]), dac8(PAL6[i][2]));
  f.setPaletteColor(IDX_WHITE, 0xff, 0xff, 0xff);
}

void FireEffect::step(lgfx::LGFX_Sprite& f) {
  uint8_t* src = fx::scratch();
  uint8_t* tmp = src + CELLS;

  for (int tick = 0; tick < TICKS_PER_FRAME; ++tick) {
    // Spread the heat: the mean of all eight neighbours. Symmetric — this
    // kernel has no idea which way is up.
    const int last = W * (HBUF - 1) - 1;
    for (int i = W + 1; i < last; ++i) {
      const int v = src[i - 1 - W] + src[i - W] + src[i + 1 - W] + src[i - 1] + src[i + 1] +
                    src[i - 1 + W] + src[i + W] + src[i + 1 + W];
      int fv = v >> 3;
      // A quarter of cells cool by one, chosen by the low bits of the sum
      // itself — no PRNG anywhere in this effect. Near the base the cooling
      // applies to cold cells too, and 0 - 1 wraps to 255: a new spark.
      if ((v & 3) == 0 && (i >= COOL_FROM || fv > 0)) fv = (fv + 255) & 0xFF;
      tmp[i] = (uint8_t)fv;
    }

    // Scroll up one row. THIS is the flame rising — not the kernel.
    memcpy(src, tmp + W, W * (HBUF - 2));

    if (blast) { // tap: a hot bar across the base, burning off over a few frames
      --blast;
      memset(tmp + W * (HBUF - 2), 200, W);
    }

    // Warm the bottom rows for display only; the simulation never sees this.
    for (int i = SMOOTH_FROM; i < W * (HBUF - 1); ++i)
      if (tmp[i] < 15) tmp[i] = (uint8_t)((256 - tmp[i] + 22) & 0xFF);
  }

  // Expand 4x into the frame, reading a row ahead as his renderer does.
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < H; ++y) {
    const uint8_t* row = tmp + (y + 1) * W;
    uint8_t* dst = fb + (y * ZOOM) * 240;
    for (int x = 0; x < W; ++x) {
      const uint8_t h = row[x];
      const uint8_t idx = h < 64 ? h : IDX_WHITE;
      memset(dst + x * ZOOM, idx, ZOOM);
    }
    for (int r = 1; r < ZOOM; ++r) memcpy(dst + r * 240, dst, 240);
  }
}

#endif
