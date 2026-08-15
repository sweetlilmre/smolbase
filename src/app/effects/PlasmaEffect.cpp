#include "PlasmaEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>

namespace {

// Three terms, each the average of two sines at unrelated frequencies, each
// scaled to 0..42 so the sum lands in 0..126 — inside the bank with no clamp
// and no modulo in the inner loop. The horizontal and vertical terms are
// separable (one lookup per column, one per row, hoisted out of the pixel
// loop); the diagonal term would not be, except that x+y for a fixed row is
// just the diagonal table read from an offset — so it costs a pointer add per
// row and stays one lookup per pixel. Three lookups, two adds, one mask, one
// store: ~2 ms for the full 240x240 field.
constexpr uint8_t AMP = 42;

// Every ramp is cyclic — first stop and last stop are the same color — because
// the rotation wraps around the bank and a mismatched pair would drag a hard
// seam through the plasma once per cycle.
const fx::Stop RAINBOW[] = {{0, 255, 0, 0},    {21, 255, 224, 0}, {43, 0, 224, 32},
                            {64, 0, 208, 224}, {85, 32, 48, 255}, {106, 224, 0, 208},
                            {127, 255, 0, 0}};
const fx::Stop EMBER[] = {{0, 16, 0, 32},    {32, 200, 32, 0},     {64, 255, 176, 40},
                          {96, 255, 240, 208}, {127, 16, 0, 32}};
const fx::Stop ICE[] = {{0, 0, 0, 40},      {32, 0, 96, 200}, {64, 80, 220, 255},
                        {96, 255, 255, 255}, {127, 0, 0, 40}};

} // namespace

void PlasmaEffect::writeScheme(lgfx::LGFX_Sprite& f) {
  switch (scheme) {
    case 1: fx::writeRamp(f, EMBER, 5, rotate); break;
    case 2: fx::writeRamp(f, ICE, 5, rotate); break;
    default: fx::writeRamp(f, RAINBOW, 7, rotate); break;
  }
}

void PlasmaEffect::enter(lgfx::LGFX_Sprite& f) {
  for (int i = 0; i < 256; ++i)
    sinTab[i] = (uint8_t)((sinf((float)i * 2.0f * (float)M_PI / 256.0f) * 0.5f + 0.5f) *
                              (float)AMP +
                          0.5f);
  writeScheme(f);
}

void PlasmaEffect::step(lgfx::LGFX_Sprite& f) {
  // The three fields drift at unrelated rates, and the palette rotates on top
  // of them at a fourth. Nothing here is synchronized to anything, which is
  // exactly why the pattern never visibly repeats.
  t1 += 3;
  t2 += 2;
  t3 += 1;
  ++rotate;
  for (int x = 0; x < 240; ++x)
    colVal[x] = (uint8_t)((sinTab[(unsigned)(x * 3 + t1) & 255] +
                           sinTab[(unsigned)(x * 7 - t2) & 255]) >>
                          1);
  for (int y = 0; y < 240; ++y)
    rowVal[y] = (uint8_t)((sinTab[(unsigned)(y * 2 + t2) & 255] +
                           sinTab[(unsigned)(y * 5 + t3) & 255]) >>
                          1);
  for (int i = 0; i < 480; ++i)
    diagVal[i] = (uint8_t)((sinTab[(unsigned)(i * 2 - t3) & 255] +
                            sinTab[(unsigned)(i + t1) & 255]) >>
                           1);

  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < 240; ++y) {
    const uint8_t r = rowVal[y];
    const uint8_t* d = diagVal + y; // the x+y diagonal, read from this row's offset
    uint8_t* p = fb + y * 240;
    for (int x = 0; x < 240; ++x) p[x] = (uint8_t)((r + colVal[x] + d[x]) & fx::BANK_MASK);
  }
  writeScheme(f);
}

#endif
