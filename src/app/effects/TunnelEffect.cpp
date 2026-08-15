#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include "assets/rings_field.h"
#include <cmath>
#include <cstring>

namespace {

constexpr int WIN = 240; // our window into the 640x400 field

// The original's pan path (Tunnel_UpdateMotion): the window spirals inward on
// a circle about the centre of the field. Its radius was 100 — the largest
// circle that keeps a 320x200 window inside 400 lines. Ours is square and
// taller, so the same rule gives 80.
constexpr int CENTRE_X = (RINGS_W - WIN) / 2; // 200
constexpr int CENTRE_Y = (RINGS_H - WIN) / 2; // 80
constexpr float START_RADIUS = (float)CENTRE_Y;
constexpr float RADIUS_FLOOR = 1.0f;

// Rates, converted from the original's frame budget. It ran locked to VGA
// retrace at ~70 Hz — 3 degrees and 0.1 of radius per frame, one fine and one
// coarse palette rotation per frame. We run at 30, so wall-clock fidelity
// means 70/30 of each per frame: the angle and radius scale directly, and the
// rotations are stepped through an accumulator because 2.33 of them is not a
// thing you can do a fractional number of times.
constexpr float HZ_RATIO = 70.0f / 30.0f;
constexpr float ANGLE_STEP = 3.0f * HZ_RATIO;
constexpr float RADIUS_DECAY = 0.1f * HZ_RATIO;

// 6-bit VGA to 8-bit: replicate the top bits rather than scaling, which is
// what the DAC effectively did. 51 -> 207, i.e. 81% — the same fraction.
inline uint8_t dac8(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

} // namespace

// Tunnel_RotateBandsFine: every 15-colour band rotates left by one, each band
// independently. Within a band that walks the ramp along the ring.
void TunnelEffect::rotateFine(uint8_t* t) {
  for (int band = 0; band < RINGS_BANDS; ++band) {
    uint8_t* b = t + band * RINGS_BAND_SIZE;
    const uint8_t first = b[0];
    memmove(b, b + 1, RINGS_BAND_SIZE - 1);
    b[RINGS_BAND_SIZE - 1] = first;
  }
}

// Tunnel_RotateBandsCoarse: the whole 225-entry table rotates right by one
// WHOLE band. That is what moves the pattern from ring to ring, and running it
// against the fine rotation is what makes the rings appear to flow inward
// rather than just shimmer in place.
void TunnelEffect::rotateCoarse(uint8_t* t) {
  uint8_t tail[RINGS_BAND_SIZE];
  memcpy(tail, t + RINGS_COLOURS - RINGS_BAND_SIZE, RINGS_BAND_SIZE);
  memmove(t + RINGS_BAND_SIZE, t, RINGS_COLOURS - RINGS_BAND_SIZE);
  memcpy(t, tail, RINGS_BAND_SIZE);
}

void TunnelEffect::uploadPalette(lgfx::LGFX_Sprite& f) {
  // Tunnel_UploadPalette: 225 colours from index 1 up, three separate channel
  // tables. On the original this had to wait for retrace or the DAC tore; here
  // the palette lands with the next present(), so there is nothing to race.
  for (int i = 0; i < RINGS_COLOURS; ++i)
    f.setPaletteColor((uint8_t)(RINGS_FIRST_INDEX + i), dac8(red[i]), dac8(green[i]),
                      dac8(blue[i]));
}

void TunnelEffect::enter(lgfx::LGFX_Sprite& f) {
  // The tables are typed constants in the original — writable, because the
  // rotations shuffle them in place. Ours live in flash, so take a working
  // copy: 675 bytes, the only RAM this effect uses.
  memcpy(red, RINGS_RED, RINGS_COLOURS);
  memcpy(green, RINGS_GREEN, RINGS_COLOURS);
  memcpy(blue, RINGS_BLUE, RINGS_COLOURS);
  angle = 0.0f;
  radius = START_RADIUS;
  inward = true;
  uploadPalette(f);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // Palette first, at the original's rate. Both rotations every step, as in
  // Scene1's loop — fine then coarse.
  rotAcc += 70;
  while (rotAcc >= 30) {
    rotAcc -= 30;
    const int8_t d = dir;
    for (uint8_t* t : {red, green, blue}) {
      if (d > 0) {
        rotateFine(t);
        rotateCoarse(t);
      } else {
        // Tap runs the whole thing backwards: the inverse of each rotation is
        // the same shuffle the other way, so the rings climb back out.
        uint8_t tmp[RINGS_BAND_SIZE];
        memcpy(tmp, t, RINGS_BAND_SIZE);
        memmove(t, t + RINGS_BAND_SIZE, RINGS_COLOURS - RINGS_BAND_SIZE);
        memcpy(t + RINGS_COLOURS - RINGS_BAND_SIZE, tmp, RINGS_BAND_SIZE);
        for (int band = 0; band < RINGS_BANDS; ++band) {
          uint8_t* b = t + band * RINGS_BAND_SIZE;
          const uint8_t last = b[RINGS_BAND_SIZE - 1];
          memmove(b + 1, b, RINGS_BAND_SIZE - 1);
          b[0] = last;
        }
      }
    }
  }
  uploadPalette(f);

  // Then the pan. The original spiralled in once and ended the scene at
  // radius 1; a clock face cannot end, so ours turns around and spirals back
  // out. That is the one deliberate departure from the reconstruction.
  const float rad = angle * (float)M_PI / 180.0f;
  const int px = (int)lroundf(radius * cosf(rad)) + CENTRE_X;
  const int py = (int)lroundf(radius * sinf(rad)) + CENTRE_Y;
  angle += ANGLE_STEP;
  if (angle > 360.0f) angle -= 360.0f;
  radius += inward ? -RADIUS_DECAY : RADIUS_DECAY;
  if (radius <= RADIUS_FLOOR) inward = false;
  if (radius >= START_RADIUS) inward = true;

  // The frame: 240 row copies straight out of flash. Not one pixel computed.
  const uint8_t* src = RINGS_FIELD + py * RINGS_W + px;
  uint8_t* dst = (uint8_t*)f.getBuffer();
  for (int y = 0; y < WIN; ++y) {
    memcpy(dst, src, WIN);
    dst += WIN;
    src += RINGS_W;
  }
}

#endif
