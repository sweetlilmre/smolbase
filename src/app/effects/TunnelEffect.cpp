#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>
#include <cstring>

namespace {

constexpr int W = fx::SCRATCH_W; // the visible half-res field
constexpr int H = fx::SCRATCH_H;
constexpr int FW = fx::TUN_W;    // the precomputed field, larger than the window
constexpr int MARGIN = fx::TUN_MARGIN;

// The wall texture: 256 texels AROUND the tunnel, 64 along it, in 8x8 cells —
// 32 cells per revolution, 8 rings per wrap. The angular axis is 256 on
// purpose: the roll advances exactly one texel per frame, and at 128 texels a
// texel is ~8 screen pixels out at the rim, so the wall visibly stuttered
// around. Finer texels make the same one-per-frame step smooth.
constexpr int T_ANG = 256;
constexpr int T_DEP = 64;
constexpr int AMASK = T_ANG - 1;
constexpr int DMASK = T_DEP - 1;
constexpr int CELL = 8;

// depth = DEPTH_K / r is the perspective, and 1/r means ring density cannot be
// right everywhere at once: spacing on screen is 8*r*r/K pixels, so any K that
// keeps the rim from stretching turns the mouth into moire. K is tuned for the
// MID field — a ring roughly every 6 half-res pixels at r = 40 — which leaves
// the rim stretched and the mouth shimmering, exactly as the reference art
// does, because it is the same geometry. Tuned against a host-side mirror of
// this file rather than by reflashing (scratchpad/tunnel_sim.py).
constexpr float DEPTH_K = 2100.0f;
// Inside R_HOLE the rings are finer than a pixel; rather than let the centre
// alias into noise it becomes the black mouth of the tunnel. Inside R_FOG the
// wall is halved into the dark half of the ramp — a two-level distance fog,
// one shift per pixel, which stops the mouth looking like a hole punched in a
// flat pattern. Both flags live in the spare bits of the depth byte.
constexpr float R_HOLE = 7.0f;
constexpr float R_FOG = 24.0f;
constexpr uint8_t HOLE = 0xFF;
constexpr uint8_t FOG = 0x40;
// The tunnel axis sits well off the centre of the field. That offset is the
// whole reason the view reads as OBLIQUE — you look down the pipe at an angle,
// the mouth rides high, and the near wall stretches away below instead of
// sitting in a tidy symmetric ring. A small offset just looks like a mistake.
constexpr float CX = (FW - 1) * 0.5f + 7.0f;
constexpr float CY = (FW - 1) * 0.5f - 26.0f;

// The drift: the window pans across the oversized field, so the vanishing
// point wanders the screen the way a hand-held camera would. Two incommensurate
// periods (~19 s and ~29 s) mean it never retraces the same path.
constexpr float PAN_X_RATE = 0.011f;
constexpr float PAN_Y_RATE = 0.0071f;

// A chunky rune, stamped black into every cell — the reference art has one too,
// and it is what makes the wall read as *surface* rather than as gradient.
const uint8_t RUNE[CELL] = {0, 0, 0b00011000, 0b00100100, 0b00111100, 0, 0, 0};
// Brightness by ring within the cell: heart, then out to the mortar. A LINEAR
// falloff was tried first and read as flat blobs — the gradient has to bite.
const uint8_t SHADE[CELL / 2] = {126, 106, 70, 18};

// ---- The palette -------------------------------------------------------------
// A textured tunnel changes what the palette is FOR. The texture carries the
// structure now, so rotating the ramp would drag the black mortar through every
// colour and strobe the whole wall — which is what an earlier cut did, and it
// looked like a flashing chessboard. The ramp is a fixed red intensity curve;
// the only thing that moves is a slow pulse in its hot end, like a light
// swinging somewhere down the pipe.
struct Rgb {
  uint8_t r, g, b;
};
constexpr int RAMP_N = 5;
const uint8_t RAMP_AT[RAMP_N] = {0, 34, 72, 104, 127};
const Rgb RAMP_RGB[RAMP_N] = {{0, 0, 0}, {88, 0, 0}, {196, 8, 0}, {248, 40, 16}, {255, 96, 48}};

} // namespace

// Each cell is bright at the heart and falls away to dark at its borders, so
// the cells read as quilted panels catching light. Flat interiors with
// one-texel mortar (an earlier cut) read as a checkerboard instead; the soft
// shading is most of the difference between "checkerboard" and "tunnel".
void TunnelEffect::buildTexture() {
  uint8_t* tex = fx::scratch() + fx::TEX_OFF;
  for (int v = 0; v < T_ANG; ++v) {
    const int cv = v % CELL;
    const float dv = fabsf((float)cv - (CELL - 1) * 0.5f);
    for (int u = 0; u < T_DEP; ++u) {
      const int cu = u % CELL;
      const float du = fabsf((float)cu - (CELL - 1) * 0.5f);
      const float m = (du > dv) ? du : dv; // 0.5 at the cell's heart, 3.5 at its edge
      uint8_t t = SHADE[(int)(m - 0.5f)];
      if (RUNE[cv] & (0x80 >> cu)) t = 0;
      tex[v * T_DEP + u] = (uint8_t)(t & fx::BANK_MASK);
    }
  }
}

void TunnelEffect::writePalette(lgfx::LGFX_Sprite& f) {
  // The hot end breathes between deep red and a hotter orange over ~14 seconds.
  const float k = 0.5f + 0.5f * sinf((float)pulse * 0.0075f);
  const Rgb top = {RAMP_RGB[RAMP_N - 1].r, (uint8_t)(RAMP_RGB[RAMP_N - 1].g + k * 84.0f),
                   (uint8_t)(RAMP_RGB[RAMP_N - 1].b + k * 40.0f)};
  int s = 0;
  for (int i = 0; i < fx::BANK_SIZE; ++i) {
    while (s + 2 < RAMP_N - 1 && i >= RAMP_AT[s + 1]) ++s;
    const Rgb& a = RAMP_RGB[s];
    const Rgb& b = (s + 1 == RAMP_N - 1) ? top : RAMP_RGB[s + 1];
    const int span = RAMP_AT[s + 1] - RAMP_AT[s];
    int t = i - RAMP_AT[s];
    if (t < 0) t = 0;
    if (t > span) t = span;
    f.setPaletteColor((uint8_t)i, (uint8_t)(a.r + (b.r - a.r) * t / span),
                      (uint8_t)(a.g + (b.g - a.g) * t / span),
                      (uint8_t)(a.b + (b.b - a.b) * t / span));
  }
}

void TunnelEffect::enter(lgfx::LGFX_Sprite& f) {
  // The one expensive moment in this effect's life: ~23 K pixels of sqrtf and
  // atan2f, paid once when the effect is switched in and never again.
  // Everything after this is two adds and a lookup.
  uint8_t* depth = fx::scratch();
  uint8_t* angle = fx::scratch() + fx::TUN_PLANE;
  for (int y = 0; y < FW; ++y) {
    const float dy = (float)y - CY;
    for (int x = 0; x < FW; ++x) {
      const float dx = (float)x - CX;
      const float r = sqrtf(dx * dx + dy * dy);
      const int i = y * FW + x;
      if (r < R_HOLE) {
        depth[i] = HOLE;
        angle[i] = 0;
        continue;
      }
      // Only the low 6 bits are kept, which costs nothing: the shift is added
      // before the wrap, and (a + s) mod 64 == ((a mod 64) + s) mod 64.
      uint8_t d = (uint8_t)((int)(DEPTH_K / r) & DMASK);
      if (r < R_FOG) d |= FOG;
      depth[i] = d;
      // atan2 spans the full texture width per revolution, so the seam at
      // +/-pi lands back on the texel it left: the wall wraps invisibly.
      angle[i] = (uint8_t)((int)((atan2f(dy, dx) + (float)M_PI) *
                                 (T_ANG / (2.0f * (float)M_PI))) &
                           AMASK);
    }
  }
  buildTexture();
  writePalette(f);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // Three counters and a texture read: that is the entire per-frame cost of a
  // tunnel. The depth shift pulls the wall toward the camera, the angle shift
  // rolls it around, and the window pans across the field so the vanishing
  // point drifts — three independent motions, none of which recomputes any
  // geometry. Both shifts advance by exactly ONE texel per frame: a fractional
  // rate would land on the same texel for two frames and then jump two, which
  // is precisely the stutter this effect had before.
  uAcc = (uint8_t)(uAcc + dir);
  vAcc = (uint8_t)(vAcc + dir);
  ++pulse;
  const uint8_t su = (uint8_t)(uAcc & DMASK), sv = vAcc;
  const int ox = MARGIN + (int)(MARGIN * sinf((float)pulse * PAN_X_RATE));
  const int oy = MARGIN + (int)(MARGIN * sinf((float)pulse * PAN_Y_RATE + 1.3f));

  const uint8_t* depth = fx::scratch();
  const uint8_t* angle = fx::scratch() + fx::TUN_PLANE;
  const uint8_t* tex = fx::scratch() + fx::TEX_OFF;
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < H; ++y) {
    uint8_t* dst = fb + (y * 2) * 240;
    const int row = (y + oy) * FW + ox;
    for (int x = 0; x < W; ++x) {
      const uint8_t d = depth[row + x];
      uint8_t t;
      if (d == HOLE) {
        t = 0;
      } else {
        // The fog bit needs no masking off: FOG is exactly T_DEP, so it
        // vanishes in the wrap (64 == 0 mod 64).
        t = tex[(((angle[row + x] + sv) & AMASK) << 6) | ((d + su) & DMASK)];
        if (d & FOG) t >>= 1; // the far end of the pipe, half a ramp darker
      }
      dst[x * 2] = t;
      dst[x * 2 + 1] = t;
    }
    memcpy(dst + 240, dst, 240); // the chunky twin row, one block copy
  }
  writePalette(f);
}

#endif
