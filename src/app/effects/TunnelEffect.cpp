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

// ---- The view is OBLIQUE, which is why this is ray casting -------------------
// A tunnel built from distance-and-angle around a screen point is always a view
// straight down the pipe: the rings come out as concentric circles, and moving
// that point around only slides the same symmetric target about. The reference
// look — rings as migrating, non-concentric arcs sweeping past a mouth that
// sits off to one side — is a genuinely tilted camera, and nothing short of
// intersecting rays with the cylinder produces it.
//
// So each field pixel casts a ray and solves for where it meets the wall.
// That is all PRECOMPUTE: any fixed camera pose costs the same at runtime,
// which is two adds and a texture lookup. The camera sits just off the axis
// and looks across the pipe; the tilt is what does the work, and hugging the
// wall (tried at 0.62 of the radius) is actively wrong — rays then strike the
// near wall at z ~ 0, where the axial coordinate barely changes, and the whole
// near field degenerates into rings-free radial streaks.
constexpr float CAM_X = 0.10f;   // camera offset from the axis, in radii
constexpr float TILT = 0.40f;    // how far the view swings across the pipe
constexpr float TAN_FOV = 0.5f;  // half-angle; wide FOVs put the wall beside us
constexpr float K_Z = 7.0f;      // texels per unit of axial distance
// Distance fade, packed into the depth byte's top two bits: near, one
// ramp-half down, one ramp-quarter down, then the black mouth. Both bits are
// multiples of T_DEP, so they vanish in the depth wrap and never need masking
// off. Three steps rather than one is what makes the mouth read as a receding
// funnel instead of a hole punched in a flat pattern.
constexpr float Z_FOG1 = 10.0f;
constexpr float Z_FOG2 = 22.0f;
constexpr float Z_HOLE = 60.0f;
constexpr uint8_t FLAGS = 0xC0;
constexpr uint8_t F1 = 0x40;
constexpr uint8_t F2 = 0x80;
constexpr uint8_t HOLE = 0xC0;

// The drift: the window pans across the oversized field, so the vanishing
// point wanders the screen the way a hand-held camera would. Two incommensurate
// periods (~19 s and ~29 s) mean it never retraces the same path.
constexpr float PAN_X_RATE = 0.011f;
constexpr float PAN_Y_RATE = 0.0071f;

// A chunky mark, stamped black into every cell — the reference art has one too,
// and it is what makes the wall read as *surface* rather than as gradient.
const uint8_t RUNE[CELL] = {0, 0, 0, 0b00011000, 0b00011000, 0, 0, 0};

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

// Each cell is a pillowed panel: a black mortar ring around the outside, then a
// smooth Euclidean falloff from a bright heart. Both halves are load-bearing.
// Flat interiors with one-texel mortar read as a checkerboard; a pure falloff
// with no mortar lets the cells bleed into one continuous red field; and a
// SQUARE falloff has only four levels in an 8x8 cell, which reads as steps.
void TunnelEffect::buildTexture() {
  uint8_t* tex = fx::scratch() + fx::TEX_OFF;
  const float edge = CELL * 0.5f - 1.2f;
  const float span = CELL * 0.58f;
  for (int v = 0; v < T_ANG; ++v) {
    const int cv = v % CELL;
    const float dv = fabsf((float)cv - (CELL - 1) * 0.5f);
    for (int u = 0; u < T_DEP; ++u) {
      const int cu = u % CELL;
      const float du = fabsf((float)cu - (CELL - 1) * 0.5f);
      uint8_t t;
      if (((du > dv) ? du : dv) > edge) {
        t = 0; // mortar
      } else {
        const float d = sqrtf(du * du + dv * dv) / span;
        const int v8 = (int)(126.0f * (1.0f - powf(d, 1.2f)));
        t = (uint8_t)(v8 < 6 ? 6 : v8);
      }
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
  // The one expensive moment in this effect's life: ~23 K rays intersected with
  // the cylinder, paid once when the effect is switched in and never again.
  // Everything after this is two adds and a lookup.
  //
  // Cylinder of radius 1 about the z axis, camera at (CAM_X, 0, 0) looking
  // along a tilted forward vector. With Fy = 0 the camera basis collapses to
  // Rt = (Fz, 0, -Fx) and U = (0, 1, 0), and the ray needs no normalising:
  // scaling the direction scales t inversely and lands on the same point.
  uint8_t* depth = fx::scratch();
  uint8_t* angle = fx::scratch() + fx::TUN_PLANE;
  const float fn = sqrtf(TILT * TILT + 1.0f);
  const float fx0 = -TILT / fn, fz0 = 1.0f / fn;
  const float pix = TAN_FOV / (W * 0.5f);
  const float c = CAM_X * CAM_X - 1.0f; // camera is inside, so c < 0: always one hit
  for (int y = 0; y < FW; ++y) {
    const float py = ((float)y - (FW - 1) * 0.5f) * pix;
    for (int x = 0; x < FW; ++x) {
      const float px = ((float)x - (FW - 1) * 0.5f) * pix;
      const float dx = fx0 + px * fz0;
      const float dy = -py;
      const float dz = fz0 - px * fx0;
      const int i = y * FW + x;
      const float a = dx * dx + dy * dy;
      if (a < 1e-7f) { // dead ahead, parallel to the axis: infinitely far
        depth[i] = HOLE;
        angle[i] = 0;
        continue;
      }
      const float b = 2.0f * CAM_X * dx;
      const float disc = b * b - 4.0f * a * c;
      const float t = (-b + sqrtf(disc > 0.0f ? disc : 0.0f)) / (2.0f * a);
      const float z = t * dz;
      // Only the FAR mouth is a hole. Rays that hit the wall behind the camera
      // have large negative z and are the nearest wall of all.
      uint8_t fl = 0;
      if (z > Z_FOG1) fl = F1;
      if (z > Z_FOG2) fl = F2;
      if (z > Z_HOLE) fl = HOLE;
      // Only the low 6 bits of depth are kept, which costs nothing: the shift
      // is added before the wrap, and (a + s) mod 64 == ((a mod 64) + s) mod 64.
      depth[i] = (uint8_t)(((int)(z * K_Z) & DMASK) | fl);
      // atan2 around the hit point spans the full texture width per
      // revolution, so the seam at +/-pi lands back on the texel it left.
      angle[i] = (uint8_t)((int)((atan2f(t * dy, CAM_X + t * dx) + (float)M_PI) *
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
      const uint8_t fl = d & FLAGS;
      uint8_t t;
      if (fl == HOLE) {
        t = 0;
      } else {
        // The fog bits need no masking off: both are multiples of T_DEP, so
        // they vanish in the wrap (64 and 128 are 0 mod 64).
        t = tex[(((angle[row + x] + sv) & AMASK) << 6) | ((d + su) & DMASK)];
        if (fl == F1) t >>= 1;      // mid distance
        else if (fl == F2) t >>= 2; // far, sliding into the mouth
      }
      dst[x * 2] = t;
      dst[x * 2 + 1] = t;
    }
    memcpy(dst + 240, dst, 240); // the chunky twin row, one block copy
  }
  writePalette(f);
}

#endif
