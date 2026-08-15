#include "RotozoomEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>

namespace {

constexpr int TEX = 64;      // power of two: wrapping is a mask, never a modulo
constexpr int TEX_MASK = 63;
constexpr int CX = 120, CY = 120;

// Copper: the metal-and-highlight ramp the era used for everything from scroll
// bars to logos. Cyclic, so rotating it walks the highlight around the texture
// instead of dragging a seam through it.
const fx::Stop COPPER[] = {{0, 24, 8, 0},      {32, 200, 96, 16}, {56, 255, 208, 128},
                           {72, 255, 255, 240}, {96, 176, 72, 8}, {127, 24, 8, 0}};

} // namespace

// Two textures, both TILING (the lookup wraps every 64 texels, so anything that
// does not tile shows a grid of seams). Variant 0 is the XOR fractal — the
// hard-edged pattern that costs one instruction and became a scene signature.
// Variant 1 is a double sine, seamless by construction, which turns the whole
// screen into moving colour bands when the palette rotates.
void RotozoomEffect::buildTexture() {
  uint8_t* tex = fx::scratch();
  for (int y = 0; y < TEX; ++y) {
    for (int x = 0; x < TEX; ++x) {
      uint8_t v;
      if (variant == 0) {
        v = (uint8_t)(((x ^ y) & TEX_MASK) * 2);
        if (((x >> 4) ^ (y >> 4)) & 1) v = (uint8_t)(126 - v); // checker inversion
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
  angle += 0.021f;
  if (angle >= 2.0f * (float)M_PI) angle -= 2.0f * (float)M_PI;
  zoomPhase += 0.013f;
  if (zoomPhase >= 2.0f * (float)M_PI) zoomPhase -= 2.0f * (float)M_PI;
  driftU = (driftU + 18000) & (TEX * 65536 - 1); // a slow pan under the
  driftV = (driftV + 11000) & (TEX * 65536 - 1); // rotation, so it never settles
  ++rotate;

  // The whole rotation and zoom: two numbers, computed once per frame. Texels
  // per screen pixel, 16.16 fixed point.
  const float z = 1.5f + 1.0f * sinf(zoomPhase);
  const int32_t ca = (int32_t)(cosf(angle) * z * 65536.0f);
  const int32_t sa = (int32_t)(sinf(angle) * z * 65536.0f);
  int32_t u0 = driftU - CX * ca - CY * sa;
  int32_t v0 = driftV + CX * sa - CY * ca;

  const uint8_t* tex = fx::scratch();
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < 240; ++y) {
    int32_t u = u0, v = v0;
    uint8_t* p = fb + y * 240;
    for (int x = 0; x < 240; ++x) {
      p[x] = tex[(((v >> 16) & TEX_MASK) << 6) | ((u >> 16) & TEX_MASK)];
      u += ca;
      v -= sa;
    }
    u0 += sa;
    v0 += ca;
  }
  fx::writeRamp(f, COPPER, 6, rotate);
}

#endif
