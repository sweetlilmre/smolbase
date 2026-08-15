#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>
#include <cstring>

namespace {

constexpr int W = fx::SCRATCH_W;
constexpr int H = fx::SCRATCH_H;

// The wall texture: 128 texels around the tunnel, 128 along it, in 8x8 cells —
// 16 cells per revolution, 16 rings per wrap. Both axes are powers of two, so
// wrapping is a mask, and 7 bits of each coordinate fit in one byte per plane.
constexpr int T = 128;
constexpr int TMASK = T - 1;
constexpr int CELL = 8;

// depth = DEPTH_K / r is the perspective, and it is the whole illusion: rings
// crowd together toward the mouth exactly as a real tunnel's do, so a CONSTANT
// shift per frame reads as constant speed down the pipe rather than as a zoom.
// K is chosen for ring DENSITY, and 1/r means you cannot have it everywhere at
// once: ring spacing on screen is 8*r*r/K pixels, so any K that keeps the
// corners from stretching turns the mouth into moire. K is therefore tuned for
// the MID field — a ring roughly every 6 half-res pixels at r = 40 — which
// leaves the corners stretched and the mouth shimmering, exactly as the
// reference art does. Tuned against a host-side mirror of this file rather
// than by reflashing (scratchpad/tunnel_sim.py).
constexpr float DEPTH_K = 2100.0f;
// Inside R_HOLE the rings are finer than a pixel; rather than let the centre
// alias into noise it becomes the black mouth of the tunnel. Inside R_FOG the
// wall is halved into the dark half of the ramp — a two-level distance fog,
// one shift per pixel, which is what stops the mouth looking like a hole
// punched in a flat pattern.
constexpr float R_HOLE = 7.0f;
constexpr float R_FOG = 24.0f;
constexpr uint8_t HOLE = 0xFF;
constexpr uint8_t FOG = 0x80;
// The vanishing point sits a little right of and below centre. Dead-centre
// reads as a target; off-centre reads as a funnel you are falling into.
constexpr float CX = (W - 1) * 0.5f + 5.0f;
constexpr float CY = (H - 1) * 0.5f + 4.0f;

// Speeds in 8.8 fixed point, per frame: the fall is brisk, the roll is slow —
// a revolution takes about eleven seconds, so the walls turn rather than spin.
constexpr int FALL = 256; // 1 texel/frame
constexpr int ROLL = 100; // ~0.39 texels/frame

// A chunky rune, stamped black into every cell — the reference art has one too,
// and it is what makes the wall read as *surface* rather than as gradient.
const uint8_t RUNE[CELL] = {0, 0, 0b00011000, 0b00100100, 0b00111100, 0, 0, 0};
// Brightness by ring within the cell: heart, then out to the mortar. A LINEAR
// falloff was tried first and read as flat blobs — the gradient has to bite.
const uint8_t SHADE[CELL / 2] = {126, 106, 70, 18};

// ---- The palette -------------------------------------------------------------
// A textured tunnel changes what the palette is FOR. The texture carries the
// structure now, so rotating the ramp would drag the black mortar through every
// colour and strobe the whole wall — which is what the first cut did, and it
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

// Each cell is bright in the middle and falls to black at its borders — a
// SQUARE falloff, so the cells read as raised quilted panels with dark mortar
// between them. Flat interiors with a one-texel border (the first cut) read as
// a checkerboard instead; the soft shading is the whole difference.
void TunnelEffect::buildTexture() {
  uint8_t* tex = fx::scratch() + fx::TEX_OFF;
  for (int v = 0; v < T; ++v) {
    const int cv = v % CELL;
    const float dv = fabsf((float)cv - (CELL - 1) * 0.5f);
    for (int u = 0; u < T; ++u) {
      const int cu = u % CELL;
      const float du = fabsf((float)cu - (CELL - 1) * 0.5f);
      const float m = (du > dv) ? du : dv; // 0.5 at the cell's heart, 3.5 at its edge
      uint8_t t = SHADE[(int)(m - 0.5f)];
      if (RUNE[cv] & (0x80 >> cu)) t = 0;
      tex[v * T + u] = (uint8_t)(t & fx::BANK_MASK);
    }
  }
}

void TunnelEffect::writePalette(lgfx::LGFX_Sprite& f) {
  // The hot end breathes between deep red and a hotter orange over ~8 seconds.
  const float k = 0.5f + 0.5f * sinf((float)pulse * 0.0075f);
  Rgb top = {(uint8_t)(RAMP_RGB[RAMP_N - 1].r),
             (uint8_t)(RAMP_RGB[RAMP_N - 1].g + k * 84.0f),
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
  // The one expensive moment in this effect's life: ~14 K pixels of sqrtf and
  // atan2f, paid once when the effect is switched in and never again.
  // Everything after this is two adds and a lookup.
  uint8_t* depth = fx::scratch();
  uint8_t* angle = fx::scratch() + fx::PLANE;
  for (int y = 0; y < H; ++y) {
    const float dy = (float)y - CY;
    for (int x = 0; x < W; ++x) {
      const float dx = (float)x - CX;
      const float r = sqrtf(dx * dx + dy * dy);
      const int i = y * W + x;
      if (r < R_HOLE) {
        depth[i] = HOLE;
        angle[i] = 0;
        continue;
      }
      // Only the low 7 bits are stored, which costs nothing: the shift is
      // added before the wrap, and (a + s) mod 128 == ((a mod 128) + s) mod 128.
      uint8_t d = (uint8_t)((int)(DEPTH_K / r) & TMASK);
      if (d == TMASK) d = TMASK - 1; // 0x7F | FOG would collide with the HOLE sentinel
      if (r < R_FOG) d |= FOG;
      depth[i] = d;
      // atan2 spans the full texture width per revolution, so the seam at
      // +/-pi lands back on the texel it left: the wall wraps invisibly.
      angle[i] = (uint8_t)((int)((atan2f(dy, dx) + (float)M_PI) *
                                 (T / (2.0f * (float)M_PI))) &
                           TMASK);
    }
  }
  buildTexture();
  writePalette(f);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // Two counters and a texture read: that is the entire per-frame cost of a
  // tunnel. The depth shift pulls the wall toward the camera, the angle shift
  // rolls it around, and because they are independent the two motions compose
  // — which is the whole reason the coordinates are kept in separate planes.
  uAcc = (uint16_t)((uAcc + dir * FALL) & 0x7FFF);
  vAcc = (uint16_t)((vAcc + dir * ROLL) & 0x7FFF);
  ++pulse;
  const uint8_t su = (uint8_t)(uAcc >> 8), sv = (uint8_t)(vAcc >> 8);

  const uint8_t* depth = fx::scratch();
  const uint8_t* angle = fx::scratch() + fx::PLANE;
  const uint8_t* tex = fx::scratch() + fx::TEX_OFF;
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < H; ++y) {
    uint8_t* dst = fb + (y * 2) * 240;
    const int row = y * W;
    for (int x = 0; x < W; ++x) {
      const uint8_t d = depth[row + x];
      uint8_t t;
      if (d == HOLE) {
        t = 0;
      } else {
        t = tex[(((angle[row + x] + sv) & TMASK) << 7) | (((d & TMASK) + su) & TMASK)];
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
