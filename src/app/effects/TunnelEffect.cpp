#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include "assets/rings_field.h"
#include <cmath>
#include <cstring>

namespace {

// The ramp the rings are read through. It must be CYCLIC — first stop and last
// stop the same colour — because rotating it wraps, and a mismatched pair would
// drag a hard seam through every ring once per cycle. The field is a sawtooth
// that wraps at 128 for exactly the same reason.
struct Rgb {
  uint8_t r, g, b;
};
constexpr int RAMP_N = 6;
const uint8_t RAMP_AT[RAMP_N] = {0, 26, 58, 84, 108, 127};
const Rgb RAMP_RGB[RAMP_N] = {{0, 0, 0}, {72, 0, 0},   {196, 8, 0},
                              {252, 56, 16}, {150, 10, 0}, {0, 0, 0}};

// One ramp step per frame at 30 Hz walks all 128 in about four seconds, near
// the rate the rings travel in the original. Whole steps only: a fractional
// rate sits still for a frame and then jumps two, which on rings this wide
// reads as a stutter.
constexpr int FLOW = 1;

// The window drifts around the field on two incommensurate periods (~35 s and
// ~51 s), so it never retraces the same path. Peak speed is about a pixel a
// frame — fast enough to read as travel, slow enough that nothing tears. The
// throat drifts in and out of view as it goes, which is the whole reason the
// original scrolled rather than sitting still.
constexpr int WIN = 240;
constexpr int PAN_X = (RINGS_W - WIN) / 2; // 200 px of swing either side
constexpr int PAN_Y = (RINGS_H - WIN) / 2; // 80
constexpr float PAN_X_RATE = 0.0060f;
constexpr float PAN_Y_RATE = 0.0041f;

} // namespace

void TunnelEffect::writePalette(lgfx::LGFX_Sprite& f) {
  // The hot end breathes over ~8 seconds — the only motion here that is not the
  // rotation itself, and enough to stop a fixed ramp looking mechanical.
  const uint8_t warm = (uint8_t)((0.5f + 0.5f * sinf((float)frame * 0.012f)) * 30.0f);
  int s = 0;
  for (int i = 0; i < fx::BANK_SIZE; ++i) {
    while (s + 2 < RAMP_N - 1 && i >= RAMP_AT[s + 1]) ++s;
    const Rgb& a = RAMP_RGB[s];
    const Rgb& b = RAMP_RGB[s + 1];
    const int span = RAMP_AT[s + 1] - RAMP_AT[s];
    int t = i - RAMP_AT[s];
    if (t < 0) t = 0;
    if (t > span) t = span;
    const int r = a.r + (b.r - a.r) * t / span;
    const int g = a.g + (b.g - a.g) * t / span;
    const int bl = a.b + (b.b - a.b) * t / span;
    f.setPaletteColor((uint8_t)((i + rotate) & fx::BANK_MASK), (uint8_t)r,
                      (uint8_t)(g + (r > 150 ? warm : 0)), (uint8_t)bl);
  }
}

void TunnelEffect::enter(lgfx::LGFX_Sprite& f) {
  // Nothing to build. No field to compute, no table to fill, no allocation to
  // make — switching into this effect is instant, where each of the ray-cast
  // versions spent tens of milliseconds here.
  writePalette(f);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // The entire frame: 240 row copies straight out of flash, and the ramp moves
  // one step. Not a single pixel is computed — the rings rush inward because
  // the colours slide beneath them, and the view travels because the window
  // moves. Two motions, no geometry, no RAM.
  ++frame;
  const int ox = PAN_X + (int)(PAN_X * sinf((float)frame * PAN_X_RATE));
  const int oy = PAN_Y + (int)(PAN_Y * sinf((float)frame * PAN_Y_RATE + 1.3f));
  const uint8_t* src = RINGS_FIELD + oy * RINGS_W + ox;
  uint8_t* dst = (uint8_t*)f.getBuffer();
  for (int y = 0; y < WIN; ++y) {
    memcpy(dst, src, WIN);
    dst += WIN;
    src += RINGS_W;
  }
  rotate = (uint8_t)((rotate + dir * FLOW) & fx::BANK_MASK);
  writePalette(f);
}

#endif
