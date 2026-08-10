#include "BoingScreen.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include "../core/Clock.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "hex_color.h"
#include <Arduino.h>
#include <cmath>
#include <ctime>

namespace {

// ---- Palette budget (research #45) ------------------------------------------
// 18 reserved indices; the other 238 keep their RGB332 identity, so color332()
// and ordinary text drawing keep working. The sacrificed RGB332 codes are the
// r=0 dark-blue/teal corner of the cube — ballIdxGuard() below keeps arbitrary
// user-picked text colors out of the reserved range.
constexpr uint8_t IDX_TRANSP = 0x00; // also black
constexpr uint8_t IDX_BALL = 0x01;   // 0x01–0x0E: the 14 cycling ball entries
constexpr uint8_t IDX_GRID = 0x0F;
constexpr uint8_t IDX_GRID_SHADOW = 0x10;
constexpr uint8_t IDX_SHADOW = 0x11;
constexpr uint8_t IDX_BG = 0x12;
constexpr uint8_t IDX_RESERVED_END = 0x12;

// A 24-bit RGB color as a framebuffer palette index: nearest RGB332 entry,
// nudged out of the reserved block (adds one green step — visually negligible).
uint8_t textIdx(uint32_t rgb) {
  uint8_t i = lgfx::color332(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff);
  if (i >= IDX_BALL && i <= IDX_RESERVED_END) i |= 0x20;
  return i;
}

// ---- Boing geometry ----------------------------------------------------------
constexpr int BALL_R = 60; // 120-px ball = the original's visual weight on 240 px
constexpr int BALL_D = BALL_R * 2;
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
constexpr uint32_t FRAME_MS = 33;  // fixed 30 Hz timestep (ticket #47)
// Spin: two cycle steps per frame ≈ 6.4°/33 ms ≈ 193°/s — the original's rate
// (one 3.2° step per NTSC vblank at 60 Hz) at our 30 Hz timestep (#54).
constexpr int SPIN_STEPS = 2;

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

void BoingScreen::loadSettings() {
  colHour = hexRgb(ConfigStore::getString("col_hour"), 0xffffff);
  colMin = hexRgb(ConfigStore::getString("col_min"), 0xffffff);
  colColon = hexRgb(ConfigStore::getString("col_colon"), 0xffffff);
  colHost = hexRgb(ConfigStore::getString("col_host"), 0xffffff);
  colIp = hexRgb(ConfigStore::getString("col_ip"), 0xffffff);
  enabled = ballOk && ConfigStore::getBool("boing"); // no ball buffer = calm screen
}

// The cycle: at any instant 7 consecutive entries are white and 7 red; one
// step shifts the pattern by one facet stripe (≈3.2° of apparent rotation).
// The entry at the white→red boundary gets the original's "reddish-white"
// blend — fake motion blur. Palette writes land at the next present().
void BoingScreen::applyCycle(lgfx::LGFX_Sprite& f) {
  for (int i = 0; i < 14; ++i) {
    int pos = (i + cyclePhase) % 14;
    if (pos == 6) f.setPaletteColor(IDX_BALL + i, 0xff, 0xdd, 0xdd);
    else if (pos < 7) f.setPaletteColor(IDX_BALL + i, 0xff, 0xff, 0xff);
    else f.setPaletteColor(IDX_BALL + i, 0xff, 0x00, 0x00);
  }
}

void BoingScreen::stepPhysics() {
  bx += vx;
  if (bx < BALL_R && vx < 0) { bx = BALL_R; vx = -vx; }
  if (bx > 240 - BALL_R && vx > 0) { bx = 240 - BALL_R; vx = -vx; }
  vy += 0.30f; // gravity
  by += vy;
  // The bounce bottoms out on the grid floor (ball bottom kisses the
  // wall/floor junction), not the screen edge — like the original.
  if (by > FLOOR_Y - BALL_R + 6 && vy > 0) { by = FLOOR_Y - BALL_R + 6; vy = -vy; } // elastic
  if (by < BALL_R && vy < 0) { by = BALL_R; vy = -vy; }
  // Spin follows travel direction, SPIN_STEPS facet stripes per frame.
  cyclePhase = (cyclePhase + (vx > 0 ? SPIN_STEPS : 14 - SPIN_STEPS)) % 14;
}

// Shadow = remap pass over the framebuffer bytes inside the shadow circle:
// grey -> dark grey, grid purple -> dark purple. Runs before the ball blit,
// so the ball is never darkened. Raw-buffer access; ~0.3 ms for an 11 K px disc.
void BoingScreen::drawShadow(lgfx::LGFX_Sprite& f, int cx, int cy) {
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

void BoingScreen::shadowString(lgfx::LGFX_Sprite& f, const String& s, int x, int y,
                               uint8_t idx) {
  f.setTextColor(IDX_TRANSP); // index 0 doubles as black in the identity palette
  f.drawString(s, x + 2, y + 2);
  f.setTextColor(idx);
  f.drawString(s, x, y);
}

void BoingScreen::drawScene(lgfx::LGFX_Sprite& f) {
  if (enabled) {
    // Inset back wall, then a SHALLOW perspective floor skirt (#54): the
    // wall's verticals fan slightly outward below the base and the few
    // floor rows spread apart as they approach — the reference frame's look.
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
      int spread = (int)((WALL_X1 - WALL_X0) / 2 * (1.0f + 0.18f * (y - FLOOR_Y) / (floorRows[3] - FLOOR_Y)));
      f.drawFastHLine(120 - spread, y, spread * 2 + 1, IDX_GRID);
    }
    drawShadow(f, (int)bx + SHADOW_DX, (int)by + SHADOW_DY);
    ball.pushSprite(&f, (int)bx - BALL_R, (int)by - BALL_R, IDX_TRANSP);
  } else {
    f.fillScreen(IDX_TRANSP); // boing off: today's calm black identity screen
  }

  // The identity overlay, every string drop-shadowed (ticket #51): drawn once
  // in black offset down-right, then in its real color on top — on an indexed
  // framebuffer that's just a second cheap text pass, and it keeps the text
  // readable over whatever the ball is doing behind it.
  // Hour and minute wear their own colors, so they are drawn as separate
  // strings hung off the colon cell; the colon blinks at 1 Hz as visible
  // proof the clock is live (solid until first NTP sync).
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  lastMinute = t.tm_min;
  colonOn = !Clock::isSynced() || (t.tm_sec & 1) == 0;
  char hh[3] = "--", mm[3] = "--";
  if (Clock::isSynced()) {
    snprintf(hh, sizeof(hh), "%02d", t.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  }
  f.setFont(&fonts::FreeSansBold24pt7b);
  int w = f.textWidth(":");
  f.setTextDatum(lgfx::middle_right);
  shadowString(f, hh, 120 - w / 2, 100, textIdx(colHour));
  f.setTextDatum(lgfx::middle_left);
  shadowString(f, mm, 120 + w / 2, 100, textIdx(colMin));
  if (colonOn) {
    f.setTextDatum(lgfx::middle_center);
    shadowString(f, ":", 120, 100, textIdx(colColon));
  }
  f.setFont(&fonts::FreeSans9pt7b);
  f.setTextDatum(lgfx::middle_center);
  shadowString(f, Net::deviceName() + ".local", 120, 160, textIdx(colHost));
  shadowString(f, Net::isUp() ? Net::ip().toString() : "connecting...", 120, 185,
               textIdx(colIp));
}

void BoingScreen::onEnter(lgfx::LGFX_Device&) {
  auto& f = Display::frame();
  f.setPaletteColor(IDX_GRID, 0xaa, 0x00, 0xaa);
  f.setPaletteColor(IDX_GRID_SHADOW, 0x66, 0x00, 0x66);
  f.setPaletteColor(IDX_SHADOW, 0x66, 0x66, 0x66);
  f.setPaletteColor(IDX_BG, 0xaa, 0xaa, 0xaa);
  dirty = true;
  lastFrameMs = 0;
}

void BoingScreen::onExit() {
  // Palette edits are global to the shared frame(): hand the reserved block
  // back as RGB332 identity so the next owner gets the documented contract.
  auto& f = Display::frame();
  for (int i = IDX_BALL; i <= IDX_RESERVED_END; ++i) {
    f.setPaletteColor(i, ((i >> 5) & 0x07) * 255 / 7, ((i >> 2) & 0x07) * 255 / 7,
                      (i & 0x03) * 255 / 3);
  }
}

void BoingScreen::tick(lgfx::LGFX_Device&) {
  auto& f = Display::frame();
  uint32_t now = millis();

  if (enabled && !paused) {
    // Fixed 30 Hz timestep (ticket #47): deterministic ball speed, and the
    // skipped passes between frames keep touch/events responsive. Catch-up
    // scheduling holds the average; a stall >1 frame resets the baseline.
    if (now - lastFrameMs < FRAME_MS) return;
    lastFrameMs += FRAME_MS;
    if (now - lastFrameMs >= FRAME_MS) lastFrameMs = now;
    if (dirty) loadSettings();
    dirty = false;
    stepPhysics();
    applyCycle(f);
    drawScene(f);
    Display::present();
    return;
  }

  // Frozen (paused or boing off): dirty-draw discipline — repaint only when
  // the displayed state changes (settings, minute rollover, colon heartbeat).
  time_t tnow = time(nullptr);
  struct tm t;
  localtime_r(&tnow, &t);
  bool colonNow = !Clock::isSynced() || (t.tm_sec & 1) == 0;
  if (!dirty && t.tm_min == lastMinute && colonNow == colonOn) return;
  if (dirty) loadSettings();
  dirty = false;
  if (enabled && !paused) { lastFrameMs = 0; return; } // settings re-enabled boing
  applyCycle(f); // frozen ball keeps its current rotation phase
  drawScene(f);
  Display::present();
}

void BoingScreen::onTap() {
  if (enabled) togglePause();
  else markDirty();
}

void BoingScreen::begin() {
  ball.setColorDepth(8);
  ballOk = ball.createSprite(BALL_D, BALL_D) != nullptr;
  if (!ballOk) return; // boot-time 14.4 KB can't realistically fail, but degrade calmly
  ball.createPalette(); // makes drawPixel/pushSprite treat colors as raw indices
  renderBall(ball);
}

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
