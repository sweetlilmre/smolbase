#include "FireEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cstring>

namespace {

// The classic three-tap blur with a random bleed-off. Each cell takes the mean
// of the three cells below it plus the one two rows down (which stretches the
// flame vertically and stops it looking like smoke), then loses a random 0..3.
// Random decay is the whole flicker: a deterministic subtraction gives a
// perfectly still flame, which reads as a photograph of fire, not fire.
constexpr int W = fx::SCRATCH_W;
constexpr int H = fx::SCRATCH_H;
constexpr uint8_t HOT = 127;

const fx::Stop FLAME[] = {{0, 0, 0, 0},        {18, 72, 0, 0},     {44, 224, 24, 0},
                          {74, 255, 148, 0},   {102, 255, 232, 96}, {127, 255, 255, 232}};

} // namespace

void FireEffect::enter(lgfx::LGFX_Sprite& f) {
  memset(fx::scratch(), 0, W * H);
  // No rotation on this one: the ramp is not cyclic (black at one end, white at
  // the other) and it is not meant to be — here the palette is a thermometer,
  // not a cycle. The animation is entirely in the heat map.
  fx::writeRamp(f, FLAME, 6, 0);
}

void FireEffect::step(lgfx::LGFX_Sprite& f) {
  uint8_t* h = fx::scratch();

  // The fuel row, below the visible flame's base: full heat with a per-cell
  // wobble, so the flame roots dance instead of standing in a line.
  uint8_t* fuel = h + (H - 1) * W;
  for (int x = 0; x < W; ++x) fuel[x] = (uint8_t)(HOT - (fx::rand8() & 31));
  if (blast) {
    // Tap: a hot blob shoved into the fuel for a few frames — the fire visibly
    // gulps and throws a tongue up the screen.
    --blast;
    const int cx = 20 + (fx::rand8() % (W - 40));
    for (int x = cx - 10; x <= cx + 10; ++x)
      if (x >= 0 && x < W) fuel[x] = HOT;
  }

  for (int y = H - 2; y >= 0; --y) {
    const uint8_t* b1 = h + (y + 1) * W;
    const uint8_t* b2 = h + ((y + 2 < H) ? (y + 2) : (H - 1)) * W;
    uint8_t* dst = h + y * W;
    for (int x = 0; x < W; ++x) {
      const int l = b1[x > 0 ? x - 1 : 0];
      const int r = b1[x < W - 1 ? x + 1 : W - 1];
      int v = (l + b1[x] + r + b2[x]) >> 2;
      v -= (fx::rand8() & 3);
      dst[x] = v > 0 ? (uint8_t)v : 0;
    }
  }
  fx::blit2x(f, 0);
}

#endif
