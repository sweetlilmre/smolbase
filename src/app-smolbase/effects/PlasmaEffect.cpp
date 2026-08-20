#include "PlasmaEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>
#include <cstring>

namespace {

constexpr uint8_t AMP = 42; // classic: three terms of 0..42 fill 0..126

// Half-res field for the two circular flavours: 120x120 expanded 2x.
constexpr int HW = 120, HH = 120;

// Circles without a square root. The table is indexed by r SQUARED (shifted
// down) and holds the sine of the root, so a ripple costs two squared-distance
// lookups, an add and one table read. At half res the largest r^2 a source can
// reach is ~1770 once shifted, hence 2048 entries.
constexpr int RING_SHIFT = 4;
constexpr int RING_N = 2048;
constexpr int SOURCES = 3;
constexpr int RING_OFF = 0;
constexpr int SQX_OFF = RING_N;                    // SOURCES rows of HW uint16
constexpr int SQY_OFF = SQX_OFF + SOURCES * HW * 2;
constexpr int SCRATCH = SQY_OFF + SOURCES * HH * 2;

// Each source contributes 0..41, so three of them land inside the 128 bank.
constexpr int RING_AMP = 41;

// Every ramp is cyclic — first stop and last stop the same colour — because
// the rotation wraps and a mismatched pair drags a seam through the field.
const fx::Stop RAINBOW[] = {{0, 255, 0, 0},    {21, 255, 224, 0}, {43, 0, 224, 32},
                            {64, 0, 208, 224}, {85, 32, 48, 255}, {106, 224, 0, 208},
                            {127, 255, 0, 0}};
const fx::Stop AURORA[] = {{0, 4, 0, 40},     {32, 0, 140, 190}, {64, 120, 255, 190},
                           {96, 190, 60, 220}, {127, 4, 0, 40}};
const fx::Stop EMBER[] = {{0, 16, 0, 32},      {32, 200, 32, 0}, {64, 255, 176, 40},
                          {96, 255, 240, 208}, {127, 16, 0, 32}};
const fx::Stop ICE[] = {{0, 0, 0, 40},      {32, 0, 96, 200}, {64, 80, 220, 255},
                        {96, 255, 255, 255}, {127, 0, 0, 40}};

// The ramp is on a clock of its own, independent of which flavour is running:
// the field and the colours change on different schedules, so the effect never
// settles into one recognisable combination. Thirty seconds each, with a short
// crossfade — a hard swap reads as a glitch, and the whole point of a palette
// this slow is that you notice it having changed rather than changing.
struct Ramp {
  const fx::Stop* stops;
  int count;
};
const Ramp RAMPS[] = {{RAINBOW, 7}, {AURORA, 5}, {EMBER, 5}, {ICE, 5}};
constexpr int RAMP_N = (int)(sizeof(RAMPS) / sizeof(RAMPS[0]));
constexpr uint16_t HOLD_FRAMES = 30 * 30; // 30 s at the fixed 30 Hz timestep
constexpr uint16_t FADE_FRAMES = 45;      // 1.5 s crossing over

inline uint16_t* row16(uint8_t* base, int off, int src, int n) {
  return (uint16_t*)(base + off) + src * n;
}

} // namespace

size_t PlasmaEffect::scratchBytes() const { return SCRATCH; }

void PlasmaEffect::writeRamp(lgfx::LGFX_Sprite& f) {
  const Ramp& from = RAMPS[rampIdx];
  const Ramp& to = RAMPS[(rampIdx + 1) % RAMP_N];
  // Blend only while crossing; the rest of the time this is one ramp write.
  const int mix = (rampHold >= HOLD_FRAMES)
                      ? (int)((rampHold - HOLD_FRAMES) * 256 / FADE_FRAMES)
                      : 0;
  for (int i = 0; i < fx::BANK_SIZE; ++i) {
    const fx::Rgb888 a = fx::rampColor(from.stops, from.count, i);
    uint8_t r = a.r, g = a.g, b = a.b;
    if (mix) {
      const fx::Rgb888 c = fx::rampColor(to.stops, to.count, i);
      r = (uint8_t)(a.r + ((c.r - a.r) * mix >> 8));
      g = (uint8_t)(a.g + ((c.g - a.g) * mix >> 8));
      b = (uint8_t)(a.b + ((c.b - a.b) * mix >> 8));
    }
    f.setPaletteColor((uint8_t)((i + rotate) & fx::BANK_MASK), r, g, b);
  }
}

// sin(sqrt(i << RING_SHIFT)) — the root is paid here, once, not per pixel.
void PlasmaEffect::buildRingTable() {
  uint8_t* ring = fx::scratch() + RING_OFF;
  for (int i = 0; i < RING_N; ++i) {
    const float r = sqrtf((float)(i << RING_SHIFT));
    ring[i] = (uint8_t)((sinf(r * 0.22f) * 0.5f + 0.5f) * RING_AMP);
  }
}

// The centres orbit on unrelated periods, which is what keeps the interference
// from ever repeating. Their squared distances are separable per axis even
// though the circles are not, so each frame costs 2 * SOURCES rows of squares
// rather than a multiply per pixel.
void PlasmaEffect::placeSources() {
  uint8_t* s = fx::scratch();
  static const float RX[SOURCES] = {0.0130f, 0.0091f, 0.0163f};
  static const float RY[SOURCES] = {0.0107f, 0.0154f, 0.0083f};
  for (int i = 0; i < SOURCES; ++i) {
    const float cx = HW * 0.5f + 46.0f * sinf((float)frame * RX[i] + (float)i);
    const float cy = HH * 0.5f + 46.0f * cosf((float)frame * RY[i] + (float)i * 2.0f);
    uint16_t* sx = row16(s, SQX_OFF, i, HW);
    uint16_t* sy = row16(s, SQY_OFF, i, HH);
    // Square in FLOAT, then quantise. Truncating the difference to an integer
    // first — which this did — snaps the whole pattern by a half-res pixel
    // every time a centre crosses a pixel boundary, and truncation toward zero
    // makes that snap asymmetric across d = 0. Both read as jitter. Squaring
    // first keeps the sub-pixel motion: the index moves by ~2*d*delta, so a
    // tenth of a pixel still shifts it smoothly out where the rings are tight.
    for (int x = 0; x < HW; ++x) {
      const float d = (float)x - cx;
      sx[x] = (uint16_t)(d * d * (1.0f / (1 << RING_SHIFT)));
    }
    for (int y = 0; y < HH; ++y) {
      const float d = (float)y - cy;
      sy[y] = (uint16_t)(d * d * (1.0f / (1 << RING_SHIFT)));
    }
  }
}

void PlasmaEffect::enter(lgfx::LGFX_Sprite& f) {
  for (int i = 0; i < 256; ++i)
    sinTab[i] = (uint8_t)((sinf((float)i * 2.0f * (float)M_PI / 256.0f) * 0.5f + 0.5f) *
                              (float)AMP +
                          0.5f);
  buildRingTable();
  writeRamp(f);
}

void PlasmaEffect::classic(lgfx::LGFX_Sprite& f) {
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
    const uint8_t* d = diagVal + y;
    uint8_t* p = fb + y * 240;
    for (int x = 0; x < 240; ++x) p[x] = (uint8_t)((r + colVal[x] + d[x]) & fx::BANK_MASK);
  }
}

// Three circular waves summed. Per pixel: three pairs of squared-distance
// lookups and three table reads — no multiply, no root, nothing separable.
void PlasmaEffect::ripples(lgfx::LGFX_Sprite& f) {
  uint8_t* s = fx::scratch();
  const uint8_t* ring = s + RING_OFF;
  const uint16_t* sx0 = row16(s, SQX_OFF, 0, HW);
  const uint16_t* sx1 = row16(s, SQX_OFF, 1, HW);
  const uint16_t* sx2 = row16(s, SQX_OFF, 2, HW);
  const uint16_t* sy0 = row16(s, SQY_OFF, 0, HH);
  const uint16_t* sy1 = row16(s, SQY_OFF, 1, HH);
  const uint16_t* sy2 = row16(s, SQY_OFF, 2, HH);

  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < HH; ++y) {
    const uint16_t a0 = sy0[y], a1 = sy1[y], a2 = sy2[y];
    uint8_t* dst = fb + (y * 2) * 240;
    for (int x = 0; x < HW; ++x) {
      const uint8_t v = (uint8_t)((ring[(a0 + sx0[x]) & (RING_N - 1)] +
                                   ring[(a1 + sx1[x]) & (RING_N - 1)] +
                                   ring[(a2 + sx2[x]) & (RING_N - 1)]) &
                                  fx::BANK_MASK);
      dst[x * 2] = v;
      dst[x * 2 + 1] = v;
    }
    memcpy(dst + 240, dst, 240);
  }
}

// One circular wave times one diagonal wave. Multiplying pinches the bands
// into nodes where either term crosses its midpoint, and because the two beat
// at different rates the nodes sweep across the field.
void PlasmaEffect::interfere(lgfx::LGFX_Sprite& f) {
  uint8_t* s = fx::scratch();
  const uint8_t* ring = s + RING_OFF;
  const uint16_t* sx = row16(s, SQX_OFF, 0, HW);
  const uint16_t* sy = row16(s, SQY_OFF, 0, HH);

  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < HH; ++y) {
    const uint16_t ay = sy[y];
    uint8_t* dst = fb + (y * 2) * 240;
    for (int x = 0; x < HW; ++x) {
      const int a = ring[(ay + sx[x]) & (RING_N - 1)] - (RING_AMP / 2);
      const int b = (int)sinTab[(unsigned)(x * 3 + y * 2 + t1) & 255] - (AMP / 2);
      const uint8_t v = (uint8_t)((((a * b) >> 3) + 64) & fx::BANK_MASK);
      dst[x * 2] = v;
      dst[x * 2 + 1] = v;
    }
    memcpy(dst + 240, dst, 240);
  }
}

void PlasmaEffect::step(lgfx::LGFX_Sprite& f) {
  // Four unrelated rates — three field phases and the ramp — so nothing in
  // here is ever synchronised to anything else, which is why the pattern does
  // not visibly repeat.
  t1 += 3;
  t2 += 2;
  t3 += 1;
  ++frame;
  ++rotate;
  if (++rampHold >= HOLD_FRAMES + FADE_FRAMES) {
    rampHold = 0;
    rampIdx = (uint8_t)((rampIdx + 1) % RAMP_N);
  }

  if (flavour == 0) {
    classic(f);
  } else {
    placeSources();
    if (flavour == 1) ripples(f);
    else interfere(f);
  }
  writeRamp(f);
}

#endif
