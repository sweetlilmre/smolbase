#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>
#include <cstring>

namespace {

constexpr int W = fx::SCRATCH_W; // the half-res field is exactly the screen
constexpr int H = fx::SCRATCH_H;

// The wall texture: 256 texels AROUND the tunnel, 64 along it, in 8x8 cells —
// 32 cells per revolution, 8 rings per wrap. The angular axis is 256 on
// purpose: the roll advances exactly one texel per frame, and at 128 texels a
// texel is ~8 screen pixels out at the rim, so the wall visibly stuttered
// around. Finer texels make the same one-per-frame step smooth.
constexpr int T_ANG = 256;
// 32 deep is not a compromise: the wall pattern repeats every CELL texels, so
// anything past one whole number of cells is duplicate bytes. 8 KB instead of
// 16 KB, pixel-for-pixel identical output, and heap is the scarce thing here.
constexpr int T_DEP = 32;
constexpr int AMASK = T_ANG - 1;
constexpr int DMASK = T_DEP - 1;
constexpr int CELL = 8;

// This effect's slice of the pool: two half-res coordinate planes then the
// wall. 37 KB, which is what keeps the device flashable while it runs.
constexpr int TEX_OFF = 2 * fx::PLANE;
constexpr int BYTES = TEX_OFF + T_ANG * T_DEP;

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
// which is two adds and a texture lookup. Hugging the wall is separately
// wrong (tried at 0.62 of the radius): rays then strike the near wall at
// z ~ 0, where the axial coordinate barely changes, and the near field
// degenerates into rings-free radial streaks.
//
// The pitch is VERTICAL, and that is the whole composition. Tilting sideways
// (tried first) puts the vanishing point out to one side at mid-height and the
// frame fills with complete concentric rings — which still reads as looking
// straight down the pipe. The reference does the opposite: mouth near the TOP
// edge and horizontally centred, the near floor sweeping toward the viewer in
// big panels, rings crossing the frame as arcs rather than closing into
// circles. That is a camera pitched DOWN relative to the tunnel axis.
// Pitch is an ANGLE from straight down the hole: 0 stares along the axis, 90
// looks flat at the horizon across the lip. It was a tangent, which cannot
// express the useful end of that range — and the useful end is near the
// horizon, because that is where the axis vanishing point leaves the frame and
// the viewer stops seeing a hole at all, which is the whole point.
constexpr float PITCH_DEG = 72.0f;
constexpr float CAM_Y = 0.45f;  // riding above the axis, so the floor runs long
constexpr float TAN_FOV = 0.5f; // half-angle; wide FOVs put the wall beside us
// The vertical FOV is WIDER than the horizontal, which squashes the view
// vertically — the reference is a 320x200 mode-13h image on a 4:3 screen, and
// without the same anamorphic squeeze the rings come out as tall ovals instead
// of the flat arcs that read as rows of floor sweeping toward the viewer. The
// vanishing point lands at TILT / (TAN_FOV * ASPECT) above centre, so these
// three move together: raise the squeeze and the pitch has to follow or the
// convergence slides back to the middle of the frame and it looks like a
// target again.
constexpr float ASPECT = 1.7f;
constexpr float K_Z = 16.0f;    // texels per unit of axial distance
// Distance fade, packed into the depth byte's top two bits as a shift count:
// 0, 1, 2, 3 ramp-halves darker. Both bits are multiples of T_DEP, so they
// vanish in the depth wrap and never need masking off, and the runtime cost is
// one shift per pixel with no branch.
//
// There is deliberately NO far clip. An earlier cut hard-cut everything past a
// distance to black, which drew a punched hole at the vanishing point — and
// what the viewer should see is the walls falling away into darkness, never a
// visible opening. The fade does that, and the fine moire where the rings
// outrun the pixel grid is the same shimmer the reference art has.
constexpr float Z1 = 4.0f;
constexpr float Z2 = 10.0f;
constexpr float Z3 = 22.0f;
constexpr uint8_t FLAGS = 0xC0;
constexpr uint8_t DEEPEST = 0xC0;

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

size_t TunnelEffect::scratchBytes() const { return BYTES; }

// Each cell is a pillowed panel: a black mortar ring around the outside, then a
// smooth Euclidean falloff from a bright heart. Both halves are load-bearing.
// Flat interiors with one-texel mortar read as a checkerboard; a pure falloff
// with no mortar lets the cells bleed into one continuous red field; and a
// SQUARE falloff has only four levels in an 8x8 cell, which reads as steps.
void TunnelEffect::buildTexture() {
  uint8_t* tex = fx::scratch() + TEX_OFF;
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
  // The one expensive moment in this effect's life: ~14 K rays intersected with
  // the cylinder, paid once when the effect is switched in and never again.
  // Everything after this is two adds and a lookup.
  //
  // Cylinder of radius 1 about the z axis; camera at (0, CAM_Y, 0) pitched down
  // by TILT. With Fx = 0 the camera basis collapses to Rt = (1, 0, 0) and
  // U = (0, Fz, -Fy), and the ray needs no normalising: scaling the direction
  // scales t inversely and lands on the same point.
  uint8_t* depth = fx::scratch();
  uint8_t* angle = fx::scratch() + fx::PLANE;
  const float th = PITCH_DEG * (float)M_PI / 180.0f;
  const float fy0 = -sinf(th), fz0 = cosf(th);
  const float pix = TAN_FOV / (W * 0.5f);
  const float pixy = pix * ASPECT;
  const float c = CAM_Y * CAM_Y - 1.0f; // camera is inside, so c < 0: always one hit
  for (int y = 0; y < H; ++y) {
    const float py = ((float)y - (H - 1) * 0.5f) * pixy;
    const float dy = fy0 - py * fz0;
    const float dz = fz0 + py * fy0;
    for (int x = 0; x < W; ++x) {
      const float dx = ((float)x - (W - 1) * 0.5f) * pix;
      const int i = y * W + x;
      const float a = dx * dx + dy * dy;
      if (a < 1e-7f) { // dead ahead, parallel to the axis: infinitely far
        depth[i] = DEEPEST;
        angle[i] = 0;
        continue;
      }
      const float b = 2.0f * CAM_Y * dy;
      const float disc = b * b - 4.0f * a * c;
      const float t = (-b + sqrtf(disc > 0.0f ? disc : 0.0f)) / (2.0f * a);
      const float z = t * dz;
      // Rays that hit the wall behind the camera have large negative z and are
      // the nearest wall of all, so only forward distance fades.
      uint8_t fl = 0;
      if (z > Z1) fl = 0x40;
      if (z > Z2) fl = 0x80;
      if (z > Z3) fl = DEEPEST;
      // Only the low bits of depth are kept, which costs nothing: the shift is
      // added before the wrap, and (a + s) mod n == ((a mod n) + s) mod n.
      depth[i] = (uint8_t)(((int)(z * K_Z) & DMASK) | fl);
      // atan2 around the hit point spans the full texture width per
      // revolution, so the seam at +/-pi lands back on the texel it left.
      angle[i] = (uint8_t)((int)((atan2f(CAM_Y + t * dy, t * dx) + (float)M_PI) *
                                 (T_ANG / (2.0f * (float)M_PI))) &
                           AMASK);
    }
  }
  buildTexture();
  writePalette(f);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // Two counters and a texture read: that is the entire per-frame cost of a
  // tunnel. The depth shift pulls the wall toward the camera and the angle
  // shift rolls it around, neither recomputing any geometry. (A third motion —
  // panning a window over an oversized field so the mouth drifts — was built
  // and then removed: the margin cost 17 KB of heap, and heap is what keeps
  // this device flashable.) Both shifts advance by exactly ONE texel per
  // frame: a fractional
  // rate would land on the same texel for two frames and then jump two, which
  // is precisely the stutter this effect had before.
  uAcc = (uint8_t)(uAcc + dir);
  vAcc = (uint8_t)(vAcc + dir);
  ++pulse;
  const uint8_t su = (uint8_t)(uAcc & DMASK), sv = vAcc;

  const uint8_t* depth = fx::scratch();
  const uint8_t* angle = fx::scratch() + fx::PLANE;
  const uint8_t* tex = fx::scratch() + TEX_OFF;
  uint8_t* fb = (uint8_t*)f.getBuffer();
  for (int y = 0; y < H; ++y) {
    uint8_t* dst = fb + (y * 2) * 240;
    const int row = y * W;
    for (int x = 0; x < W; ++x) {
      // The fade bits need no masking off: both are multiples of T_DEP, so
      // they vanish in the depth wrap. Branchless — the top two bits ARE the
      // shift count.
      const uint8_t d = depth[row + x];
      const uint8_t t =
          (uint8_t)(tex[(((angle[row + x] + sv) & AMASK) * T_DEP) | ((d + su) & DMASK)] >>
                    ((d & FLAGS) >> 6));
      dst[x * 2] = t;
      dst[x * 2 + 1] = t;
    }
    memcpy(dst + 240, dst, 240); // the chunky twin row, one block copy
  }
  writePalette(f);
}

#endif
