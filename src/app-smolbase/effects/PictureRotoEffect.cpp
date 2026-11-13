#include "PictureRotoEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>
#include <cstring>

namespace {

constexpr int TEX = fx::TEX_W; // power of two: wrapping is a mask, never a modulo
constexpr int TEX_MASK = fx::TEX_MASK;
constexpr int CX = 120, CY = 120; // screen centre; the pivot swings about it

// ---- The palette -------------------------------------------------------------
// All 128 entries of the bank, exactly filled: 1 + 86 + 41. An earlier cut used
// 64 and left the top half idle, which banded visibly across the big sphere at
// full magnification -- 39 steps cannot carry a gradient over 200 px. Three
// groups, because a picture needs materials rather than one ramp: index 0 is
// the background and the tile border, and the two ramps below shade the two
// kinds of object in the stand-in. A real asset replaces all of this with its
// own table and nothing else here changes.
constexpr uint8_t IDX_BG = 0;
constexpr uint8_t IDX_WARM = 1;  // .. IDX_WARM + WARM_N - 1
constexpr int WARM_N = 86;
constexpr uint8_t IDX_COOL = 87; // .. IDX_COOL + COOL_N - 1  (== 127, the bank's last)
constexpr int COOL_N = 41;

struct Key {
  float at; // 0..1 across the ramp
  uint8_t r, g, b;
};

// Copper through to a hot highlight — the metal the scene painted everything in.
const Key WARM[] = {{0.00f, 16, 4, 10},    {0.18f, 92, 20, 16},
                    {0.38f, 170, 62, 20},  {0.60f, 226, 124, 44},
                    {0.80f, 248, 186, 104}, {0.93f, 255, 226, 170},
                    {1.00f, 255, 252, 240}};
// Steel, so the satellites read as a different material and the eye can tell
// which part of the tile it is looking at when the wrap repeats them.
const Key COOL[] = {{0.00f, 8, 12, 28},    {0.30f, 40, 58, 96},
                    {0.62f, 96, 124, 168}, {0.85f, 158, 186, 224},
                    {1.00f, 222, 236, 255}};

// Counted from the arrays, never written out: a ramp gains a key and the loops
// below follow it, which a literal would not.
constexpr int WARM_KEYS = (int)(sizeof(WARM) / sizeof(WARM[0]));
constexpr int COOL_KEYS = (int)(sizeof(COOL) / sizeof(COOL[0]));

void sampleKeys(const Key* keys, int n, float t, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (t <= keys[0].at) {
    r = keys[0].r, g = keys[0].g, b = keys[0].b;
    return;
  }
  for (int i = 1; i < n; ++i) {
    if (t > keys[i].at) continue;
    const float span = keys[i].at - keys[i - 1].at;
    const float f = span > 0.0f ? (t - keys[i - 1].at) / span : 0.0f;
    r = (uint8_t)(keys[i - 1].r + (keys[i].r - keys[i - 1].r) * f);
    g = (uint8_t)(keys[i - 1].g + (keys[i].g - keys[i - 1].g) * f);
    b = (uint8_t)(keys[i - 1].b + (keys[i].b - keys[i - 1].b) * f);
    return;
  }
  r = keys[n - 1].r, g = keys[n - 1].g, b = keys[n - 1].b;
}

// ---- The transform -----------------------------------------------------------
// z is texels per screen pixel, so the whole 128-texel tile spans the 240 px
// screen at z = 128/240 = 0.533, and the screen shows 240*z/128 copies of it.
// The range below runs from 0.35 (the subject magnified to about 1.5 screens,
// still legible) to 2.20 (about 4.1 tiles across, a full grid of them).
// It never approaches zero: RotozoomEffect sweeps through zero deliberately,
// because one texel filling the screen is a signature when the texture is a
// pattern and a mess when it is a face.
// These four were tuned against a host mirror of this file (same constants,
// same 16.16 maths, rendered to PNG) rather than by reflashing: the panel is
// OTA-only, so a round trip costs minutes and ends with someone else having to
// look at it. Measured over one zoom cycle the screen shows 0.66 to 4.13 copies
// of the tile, and the subject stays legible at the near end.
constexpr float ZOOM_MID = 1.275f;
constexpr float ZOOM_AMP = 0.925f;
constexpr float ANGLE_STEP = 1.6f; // deg/frame -> 7.5 s per revolution
constexpr float ZOOM_STEP = 1.1f;  // deg/frame -> 10.9 s per zoom cycle

// ---- The pivot ---------------------------------------------------------------
// The still point of the rotation, which MOVES. That is the whole of the pan
// here, and it took two wrong turns to get to:
//
//   1. Drifting the texture coordinate by a constant in TEXTURE space. On
//      screen that is -R^-1 dd/dt: a velocity whose direction rotates with the
//      texture and whose magnitude scales as 1/z, so the picture circled the
//      centre instead of travelling, and nearly stalled when the zoom pulled
//      out.
//   2. Drifting by a constant in SCREEN space, dd = R v, which makes the
//      instantaneous velocity exactly -v. Correct per frame and useless over
//      time: d then accumulates sum(R(theta_k) v), and a rotating vector summed
//      over a full turn is ZERO. Traced on the host mirror the net displacement
//      came out 0.000 px/frame over one, two and four revolutions.
//
// Both fight the same hard limit: the texture tiles, so sliding its phase is
// only visible modulo one tile period -- 58 px at full zoom-out, which the pan
// crossed in 2.2 s. Translating a periodic pattern cannot read as travel.
//
// Moving the PIVOT is not subject to that, because what moves is the motion
// field rather than the phase. The eye locks onto the still point of a
// rotation, and when that point slides across the panel the whole field sweeps
// with it. Two sinusoids on incommensurate periods, so the pivot wanders a
// Lissajous figure rather than retracing a circle; the amplitude deliberately
// carries it off the panel, so the effect alternates between a pinwheel you
// can see the centre of and a sweep across one corner.
constexpr float PIVOT_AMP_X = 140.0f; // px about the screen centre (panel is 240)
constexpr float PIVOT_AMP_Y = 140.0f;
constexpr float PIVOT_STEP_X = 0.60f; // deg/frame -> 20.0 s, peak 44 px/s
constexpr float PIVOT_STEP_Y = 0.79f; // deg/frame -> 15.2 s, peak 58 px/s

// Shade one point on a sphere of radius `rad` offset (dx, dy) from its centre.
// Returns -1.0 outside the sphere, otherwise 0..1 ready to index a ramp.
float sphereShade(float dx, float dy, float rad) {
  const float d2 = dx * dx + dy * dy;
  const float r2 = rad * rad;
  if (d2 > r2) return -1.0f;
  // The z of the unit normal; x and y are already the normal, scaled.
  const float nx = dx / rad, ny = dy / rad;
  const float nz = sqrtf(1.0f - (d2 / r2));
  // One light, up and to the left, normalised by hand.
  constexpr float LX = -0.485f, LY = -0.571f, LZ = 0.663f;
  float diff = nx * LX + ny * LY + nz * LZ;
  if (diff < 0.0f) diff = 0.0f;
  // A cheap specular: the diffuse term raised to a high power peaks tightly
  // where the normal faces the light, which is all a 64-colour ramp can show.
  const float d4 = diff * diff * diff * diff;
  const float spec = d4 * d4 * d4;
  float v = 0.10f + 0.72f * diff + 0.85f * spec;
  if (v > 1.0f) v = 1.0f;
  return v;
}

} // namespace

// A centred subject on a black field, which is what makes a picture tile: the
// border is uniform, so the masked wrap has nothing to show at the seam. The
// subject is a shaded sphere (smooth gradient, to see how the ramp bands) with
// four smaller ones off-centre (fine detail, to see how the transform aliases
// when the zoom minifies). Everything stays at least 18 px clear of the edge.
void PictureRotoEffect::buildStandIn() {
  uint8_t* tex = fx::scratch();
  memset(tex, IDX_BG, TEX * TEX);

  constexpr float MAIN_R = 40.0f;
  constexpr float SAT_R = 8.0f;
  constexpr int SAT[4][2] = {{26, 26}, {102, 26}, {26, 102}, {102, 102}};

  for (int y = 0; y < TEX; ++y) {
    for (int x = 0; x < TEX; ++x) {
      const float v = sphereShade((float)x - 64.0f, (float)y - 64.0f, MAIN_R);
      if (v >= 0.0f) {
        tex[y * TEX + x] = (uint8_t)(IDX_WARM + (int)(v * (WARM_N - 1) + 0.5f));
        continue;
      }
      for (const auto& s : SAT) {
        const float sv = sphereShade((float)x - s[0], (float)y - s[1], SAT_R);
        if (sv >= 0.0f) {
          tex[y * TEX + x] = (uint8_t)(IDX_COOL + (int)(sv * (COOL_N - 1) + 0.5f));
          break;
        }
      }
    }
  }
}

// Written ONCE, in enter(). The whole point of this scene is that the palette
// does not move: the picture keeps its own colours and the motion is entirely
// in the transform. step() touches no palette entry at all, which also saves
// the 128 setPaletteColor calls a cycling effect pays every frame.
void PictureRotoEffect::writePalette(lgfx::LGFX_Sprite& f) {
  f.setPaletteColor(IDX_BG, 0, 0, 0);
  uint8_t r, g, b;
  for (int i = 0; i < WARM_N; ++i) {
    sampleKeys(WARM, WARM_KEYS, (float)i / (WARM_N - 1), r, g, b);
    f.setPaletteColor((uint8_t)(IDX_WARM + i), r, g, b);
  }
  for (int i = 0; i < COOL_N; ++i) {
    sampleKeys(COOL, COOL_KEYS, (float)i / (COOL_N - 1), r, g, b);
    f.setPaletteColor((uint8_t)(IDX_COOL + i), r, g, b);
  }
}

void PictureRotoEffect::enter(lgfx::LGFX_Sprite& f) {
  // Unconditionally, every time: the scratch pool is freed on switch-out and
  // allocated again on switch-in, so a one-shot build flag would paint the
  // second visit out of whatever now owns that heap (see Effect.h).
  buildStandIn();
  writePalette(f);
}

void PictureRotoEffect::step(lgfx::LGFX_Sprite& f) {
  // Every accumulator is wrapped rather than left to grow: this screen runs for
  // months at 30 Hz. The angles wrap at a full turn and the pan at one texture
  // width, which keeps the 16.16 accumulators clear of signed overflow.
  angle += ANGLE_STEP * spin;
  if (angle >= 360.0f) angle -= 360.0f;
  if (angle < 0.0f) angle += 360.0f;
  zoomPhase += ZOOM_STEP;
  if (zoomPhase >= 360.0f) zoomPhase -= 360.0f;
  pivotPhaseX += PIVOT_STEP_X;
  if (pivotPhaseX >= 360.0f) pivotPhaseX -= 360.0f;
  pivotPhaseY += PIVOT_STEP_Y;
  if (pivotPhaseY >= 360.0f) pivotPhaseY -= 360.0f;

  // Two numbers per frame, exactly as in RotozoomEffect: the transform is
  // affine, so the texture coordinate at pixel x+1 is the one at x plus a
  // constant, and sin/cos are called twice per FRAME rather than per pixel.
  const float rad = angle * (float)M_PI / 180.0f;
  const float z = ZOOM_MID + ZOOM_AMP * sinf(zoomPhase * (float)M_PI / 180.0f);
  const float cz = cosf(rad) * z, sz = sinf(rad) * z;
  const int32_t ca = (int32_t)(cz * 65536.0f);
  const int32_t sa = (int32_t)(sz * 65536.0f);

  // t = R (p - pivot): the only change from a fixed-centre rotozoom is that the
  // subtracted point moves. No accumulator, so nothing to wrap and nothing to
  // drift out of precision -- the pivot is recomputed from its phase every
  // frame, and the phases are the only state.
  const float pivX = CX + PIVOT_AMP_X * sinf(pivotPhaseX * (float)M_PI / 180.0f);
  const float pivY = CY + PIVOT_AMP_Y * sinf(pivotPhaseY * (float)M_PI / 180.0f);
  int32_t u0 = (int32_t)((-pivX * cz - pivY * sz) * 65536.0f);
  int32_t v0 = (int32_t)((pivX * sz - pivY * cz) * 65536.0f);

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
}

#endif
