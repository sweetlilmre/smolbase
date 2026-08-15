#include "BoingEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>

namespace {

// ---- Bank budget (research #45) ---------------------------------------------
// 19 of the effect bank's 128 entries; the rest go unused. Index 0 doubles as
// the ball sprite's transparent color and as black.
constexpr uint8_t IDX_TRANSP = 0x00;
constexpr uint8_t IDX_BALL = 0x01; // 0x01-0x0E: the 14 cycling ball entries
constexpr uint8_t IDX_GRID = 0x0F;
constexpr uint8_t IDX_GRID_SHADOW = 0x10;
constexpr uint8_t IDX_SHADOW = 0x11;
constexpr uint8_t IDX_BG = 0x12;

// ---- Boing geometry ----------------------------------------------------------
constexpr int BALL_R = 60; // 120-px ball = the original's visual weight on 240 px
constexpr int BALL_D = BALL_R * 2;
static_assert(BALL_D == fx::SCRATCH_W && BALL_D == fx::SCRATCH_H,
              "the ball is pre-rendered into the shared scratch");
constexpr float TILT_DEG = -16.0f; // axis leans RIGHT, per the reference video (#54)
constexpr int GRID_STEP = 16;      // ~the original's 16-cell wall
// The wall is INSET — grey margins all around, like the reference frame (#54):
// 12 cells wide (24 px margins), top margin 8 px, base at FLOOR_Y with a
// shallow perspective skirt of a few spreading rows below it.
constexpr int WALL_X0 = 24, WALL_X1 = 216, WALL_Y0 = 8;
constexpr int FLOOR_Y = 208;       // wall/floor junction: the ball bounces off this
// Shadow: cast onto the wall behind, right of the ball and a hair above
// level, ball-sized — user-tuned against the reference frame (#54).
constexpr int SHADOW_DX = 24, SHADOW_DY = -2;
// Spin: two cycle steps per frame ~ 6.4 deg/33 ms ~ 193 deg/s — the original's
// rate (one 3.2 deg step per NTSC vblank at 60 Hz) at our 30 Hz timestep (#54).
constexpr int SPIN_STEPS = 2;
constexpr float GRAVITY = 0.30f;         // px/frame^2 at the fixed timestep
constexpr float BASE_APEX_Y = 90.0f;     // the natural bounce apex (the boot drop height)
constexpr int FLOOR_CONTACT_Y = FLOOR_Y - BALL_R + 6; // ball center at floor kiss
// Tap kick (ticket #60, reworked #100): the energy a tap adds, and how fast
// that extra energy bleeds off. Without the decay a kicked apex would persist
// forever; instead each floor contact keeps at most DAMP of any rebound speed
// above the natural one, settling back to the regular bounce in a few hops.
// The rebound never drops below natural (#61). The kick is a flat speed add
// along the current direction of travel (#100): direction-following is what
// fixed the old clamped feel (an upward-only kick subtracted energy from a
// falling ball, so rapid taps half-cancelled), and the linear add keeps the
// buildup gradual — multiplicative gains (1.15, then 1.08) were tried
// on-device and escalated too fast. Damping drains a fixed fraction per
// contact and contacts get more frequent with speed, so the buildup is
// self-limiting with no explicit cap.
constexpr float KICK_VY = 4.0f;
constexpr float KICK_DAMP = 0.85f;

// The original's startup act: draw the tilted checkered sphere ONCE, as palette
// indices. Per pixel: project onto the front hemisphere, tilt, and apply the
// original's facet formula ((lat&1)*7 + lon) % 14 across 8 latitude bands and
// 56 longitude facets — so the 14-entry cycle animates it unchanged.
void renderBall(lgfx::LGFX_Sprite& ball) {
  const float tilt = TILT_DEG * (float)M_PI / 180.0f;
  const float ct = cosf(tilt), st = sinf(tilt);
  for (int y = 0; y < BALL_D; ++y) {
    for (int x = 0; x < BALL_D; ++x) {
      float dx = x - (BALL_R - 0.5f), dy = y - (BALL_R - 0.5f);
      float r2 = dx * dx + dy * dy;
      if (r2 > (float)(BALL_R * BALL_R)) {
        ball.drawPixel(x, y, IDX_TRANSP);
        continue;
      }
      float z = sqrtf((float)(BALL_R * BALL_R) - r2);
      float tx = dx * ct - dy * st, ty = dx * st + dy * ct; // tilt = screen-plane rotation
      float lat = asinf(ty / BALL_R);                 // [-pi/2, pi/2]
      float lon = atan2f(tx, z);                      // (-pi/2, pi/2), front hemisphere
      int band = (int)((lat / (float)M_PI + 0.5f) * 8.0f);
      int facet = (int)((lon / (float)M_PI + 0.5f) * 56.0f);
      if (band > 7) band = 7;
      if (facet > 55) facet = 55;
      ball.drawPixel(x, y, IDX_BALL + (uint8_t)(((band & 1) * 7 + facet) % 14));
    }
  }
}

} // namespace

// The cycle: at any instant 7 consecutive entries are white and 7 red; one
// step shifts the pattern by one facet stripe (~3.2 deg of apparent rotation).
// The entry at the white->red boundary gets the original's "reddish-white"
// blend — fake motion blur. Palette writes land at the next present().
void BoingEffect::applyCycle(lgfx::LGFX_Sprite& f) {
  for (int i = 0; i < 14; ++i) {
    int pos = (i + cyclePhase) % 14;
    if (pos == 6) f.setPaletteColor(IDX_BALL + i, 0xff, 0xdd, 0xdd);
    else if (pos < 7) f.setPaletteColor(IDX_BALL + i, 0xff, 0xff, 0xff);
    else f.setPaletteColor(IDX_BALL + i, 0xff, 0x00, 0x00);
  }
}

void BoingEffect::stepPhysics() {
  bx += vx;
  if (bx < BALL_R && vx < 0) { bx = BALL_R; vx = -vx; }
  if (bx > 240 - BALL_R && vx > 0) { bx = 240 - BALL_R; vx = -vx; }
  vy += GRAVITY;
  by += vy;
  // The bounce bottoms out on the grid floor (ball bottom kisses the
  // wall/floor junction), not the screen edge — like the original. The rebound
  // is clamped to at least the natural speed (a fall from BASE_APEX_Y): a tap
  // caught mid-fall bleeds arrival speed, and an elastic bounce would keep
  // that low apex forever (#61). Anything above natural is tap-kick energy
  // (#60) and decays by KICK_DAMP per contact.
  if (by > FLOOR_CONTACT_Y && vy > 0) {
    by = FLOOR_CONTACT_Y;
    const float natural = sqrtf(2.0f * GRAVITY * (FLOOR_CONTACT_Y - BASE_APEX_Y));
    vy = -fmaxf(natural, vy * KICK_DAMP);
  }
  if (by < BALL_R && vy < 0) { by = BALL_R; vy = -vy; }
  // Spin follows travel direction, SPIN_STEPS facet stripes per frame.
  cyclePhase = (cyclePhase + (vx > 0 ? SPIN_STEPS : 14 - SPIN_STEPS)) % 14;
}

// Shadow = remap pass over the framebuffer bytes inside the shadow circle:
// grey -> dark grey, grid purple -> dark purple. Runs before the ball blit,
// so the ball is never darkened. Raw-buffer access; ~0.3 ms for an 11 K px disc.
void BoingEffect::drawShadow(lgfx::LGFX_Sprite& f, int cx, int cy) {
  uint8_t* fb = (uint8_t*)f.getBuffer();
  const int r = BALL_R;
  for (int yy = cy - r; yy <= cy + r; ++yy) {
    if (yy < 0 || yy > 239) continue;
    int hw = (int)sqrtf((float)(r * r - (yy - cy) * (yy - cy)));
    int x0 = cx - hw, x1 = cx + hw;
    if (x0 < 0) x0 = 0;
    if (x1 > 239) x1 = 239;
    uint8_t* row = fb + yy * 240;
    for (int xx = x0; xx <= x1; ++xx) {
      if (row[xx] == IDX_BG) row[xx] = IDX_SHADOW;
      else if (row[xx] == IDX_GRID) row[xx] = IDX_GRID_SHADOW;
    }
  }
}

void BoingEffect::enter(lgfx::LGFX_Sprite& f) {
  if (!spriteReady) {
    // Wrapped around the shared scratch rather than allocated: setBuffer takes
    // no ownership, so nothing here is ever freed or can fail. createPalette
    // is what makes drawPixel/pushSprite treat colors as raw indices, and it
    // is the sprite's only (1 KB, one-time) allocation.
    ball.setColorDepth(8);
    ball.setBuffer(fx::scratch(), BALL_D, BALL_D, 8);
    ball.createPalette();
    spriteReady = true;
  }
  renderBall(ball); // the scratch is shared: re-render on every switch-in
  f.setPaletteColor(IDX_TRANSP, 0x00, 0x00, 0x00);
  f.setPaletteColor(IDX_GRID, 0xaa, 0x00, 0xaa);
  f.setPaletteColor(IDX_GRID_SHADOW, 0x66, 0x00, 0x66);
  f.setPaletteColor(IDX_SHADOW, 0x66, 0x66, 0x66);
  f.setPaletteColor(IDX_BG, 0xaa, 0xaa, 0xaa);
  applyCycle(f);
}

void BoingEffect::step(lgfx::LGFX_Sprite& f) {
  stepPhysics();
  applyCycle(f); // 14 palette writes = the whole rotation
  // Inset back wall, then a SHALLOW perspective floor skirt (#54): the wall's
  // verticals fan slightly outward below the base and the few floor rows
  // spread apart as they approach — the reference frame's look.
  f.fillScreen(IDX_BG);
  for (int x = WALL_X0; x <= WALL_X1; x += GRID_STEP)
    f.drawFastVLine(x, WALL_Y0, FLOOR_Y - WALL_Y0 + 1, IDX_GRID);
  for (int y = WALL_Y0; y <= FLOOR_Y; y += GRID_STEP)
    f.drawFastHLine(WALL_X0, y, WALL_X1 - WALL_X0 + 1, IDX_GRID);
  f.drawFastHLine(WALL_X0, FLOOR_Y, WALL_X1 - WALL_X0 + 1, IDX_GRID); // base
  static const int floorRows[] = {212, 216, 221, 227};
  for (int x = WALL_X0; x <= WALL_X1; x += GRID_STEP) {
    int x2 = 120 + (int)((x - 120) * 1.18f);
    f.drawLine(x, FLOOR_Y, x2, floorRows[3], IDX_GRID);
  }
  for (int y : floorRows) {
    int spread = (int)((WALL_X1 - WALL_X0) / 2 *
                       (1.0f + 0.18f * (y - FLOOR_Y) / (floorRows[3] - FLOOR_Y)));
    f.drawFastHLine(120 - spread, y, spread * 2 + 1, IDX_GRID);
  }
  drawShadow(f, (int)bx + SHADOW_DX, (int)by + SHADOW_DY);
  ball.pushSprite(&f, (int)bx - BALL_R, (int)by - BALL_R, IDX_TRANSP);
}

void BoingEffect::onTap() {
  float s = fabsf(vy) + KICK_VY;
  vy = (vy < 0) ? -s : s;
}

#endif
