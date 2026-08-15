#include "RotozoomEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>

namespace {

constexpr int TEX = fx::TEX_W; // power of two: wrapping is a mask, never a modulo
constexpr int TEX_MASK = fx::TEX_MASK;
constexpr int CX = 120, CY = 120;

// One angle drives the spin and the zoom together: scale = sin(angle) + 1, so
// the zoom completes exactly one cycle per revolution and sweeps down THROUGH
// zero. That sweep is the signature — as the scale approaches nothing, a single
// texel magnifies to fill the screen and the whole frame resolves into one
// slab of colour before opening back out. An earlier cut drove the zoom from
// its own phase over a range that never came near zero, and it just looked
// like a texture drifting about.
constexpr float ANGLE_STEP = 1.2f;  // degrees per frame: ~10 s per revolution
constexpr float SCALE_FLOOR = 0.06f; // one texel across ~17 px, not a dead frame

// Copper: the metal-and-highlight ramp the era used for everything from scroll
// bars to logos. Cyclic, so rotating it walks the highlight around the texture
// instead of dragging a seam through it.
const fx::Stop COPPER[] = {{0, 24, 8, 0},      {32, 200, 96, 16}, {56, 255, 208, 128},
                           {72, 255, 255, 240}, {96, 176, 72, 8}, {127, 24, 8, 0}};

} // namespace

// Two textures, both TILING (the lookup wraps every 128 texels, so anything
// that does not tile shows a grid of seams). Variant 0 is the XOR fractal — the
// hard-edged pattern that costs one instruction and became a scene signature.
// Variant 1 is a double sine, seamless by construction, which turns the whole
// screen into moving colour bands when the palette rotates.
void RotozoomEffect::buildTexture() {
  uint8_t* tex = fx::scratch();
  for (int y = 0; y < TEX; ++y) {
    for (int x = 0; x < TEX; ++x) {
      uint8_t v;
      if (variant == 0) {
        // The XOR fractal, one instruction and an unmistakable scene
        // signature. At 128 wide it lands on the bank exactly, no scaling.
        v = (uint8_t)((x ^ y) & TEX_MASK);
        if (((x >> 5) ^ (y >> 5)) & 1) v = (uint8_t)(TEX_MASK - v); // checker inversion
      } else {
        const float s = sinf((float)x * 2.0f * (float)M_PI / TEX) +
                        sinf((float)y * 2.0f * (float)M_PI / TEX);
        v = (uint8_t)((s * 0.25f + 0.5f) * 127.0f);
      }
      tex[y * TEX + x] = (uint8_t)(v & fx::BANK_MASK);
    }
  }
}

void RotozoomEffect::enter(lgfx::LGFX_Sprite& f) {
  buildTexture();
  rebuild = false;
  fx::writeRamp(f, COPPER, 6, rotate);
}

void RotozoomEffect::step(lgfx::LGFX_Sprite& f) {
  if (rebuild) {
    buildTexture();
    rebuild = false;
  }
  // Every accumulator here is wrapped, not left to grow: this screen runs for
  // months at 30 Hz. The angles wrap at a full turn (sinf/cosf lose precision
  // long before they lose meaning), and the pan wraps at one texture width,
  // which is exactly a no-op for a lookup that masks with & 63 anyway — but it
  // keeps the 16.16 accumulator well clear of signed overflow.
  angle += ANGLE_STEP;
  if (angle >= 360.0f) angle -= 360.0f;
  driftU = (driftU + 18000) & (TEX * 65536 - 1); // a slow pan under the
  driftV = (driftV + 11000) & (TEX * 65536 - 1); // rotation, so it never settles
  ++rotate;

  // The whole rotation and zoom: two numbers, computed once per frame. Texels
  // per screen pixel, 16.16 fixed point. Because the transform is affine, the
  // texture coordinate at pixel x+1 is the one at x plus a constant — so sin
  // and cos are called twice per FRAME, never per pixel.
  const float rad = angle * (float)M_PI / 180.0f;
  const float sn = sinf(rad);
  const float z = fmaxf(sn + 1.0f, SCALE_FLOOR);
  const int32_t ca = (int32_t)(cosf(rad) * z * 65536.0f);
  const int32_t sa = (int32_t)(sn * z * 65536.0f);
  int32_t u0 = driftU - CX * ca - CY * sa;
  int32_t v0 = driftV + CX * sa - CY * ca;

  const uint8_t* tex = fx::scratch();
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < 240; ++y) {
    int32_t u = u0, v = v0;
    uint8_t* p = fb + y * 240;
    for (int x = 0; x < 240; ++x) {
      p[x] = tex[(((v >> 16) & TEX_MASK) << fx::TEX_SHIFT) | ((u >> 16) & TEX_MASK)];
      u += ca;
      v -= sa;
    }
    u0 += sa;
    v0 += ca;
  }
  fx::writeRamp(f, COPPER, 6, rotate);
}

#endif
