#include "TunnelEffect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cmath>

namespace {

constexpr int W = fx::SCRATCH_W;
constexpr int H = fx::SCRATCH_H;

// depth = DEPTH_K / r is the perspective: rings crowd together toward the
// center exactly as a real tunnel's do, which is why a CONSTANT phase step
// reads as constant speed down the pipe rather than as a zoom. Below R_MIN the
// rings are finer than a pixel and the center becomes a shimmering
// singularity — every tunnel of the era did that, and clamping keeps the
// shimmer to a dozen pixels instead of a quarter of the screen.
constexpr float DEPTH_K = 5000.0f;
constexpr float R_MIN = 3.0f;
// The angle spans the FULL 128-entry bank per revolution, so the wrap from
// 359 degrees back to 0 lands on the same palette entry it left: no seam. The
// side effect is a one-armed spiral, which is the look this effect is after.
constexpr float ANG_SCALE = 128.0f / (2.0f * (float)M_PI);

constexpr int8_t FLOW = 2;   // field steps per frame — the rush down the pipe
constexpr int8_t CRAWL = 1;  // ramp steps per frame, against the flow

// Cyclic (the first and last stop match): the ramp is rotated every frame, so
// a mismatch would drag a hard seam around the tunnel once per cycle.
const fx::Stop WORMHOLE[] = {{0, 8, 0, 24},     {26, 128, 0, 160}, {58, 255, 48, 168},
                             {84, 64, 224, 255}, {104, 255, 255, 255}, {127, 8, 0, 24}};

} // namespace

void TunnelEffect::enter(lgfx::LGFX_Sprite& f) {
  // The one expensive moment in this effect's life: ~14 K pixels of sqrtf and
  // atan2f, about 20 ms, paid once when the effect is switched in and never
  // again. Everything after this is an add.
  uint8_t* fld = fx::scratch();
  for (int y = 0; y < H; ++y) {
    const float dy = (float)y - (H - 1) * 0.5f;
    for (int x = 0; x < W; ++x) {
      const float dx = (float)x - (W - 1) * 0.5f;
      float r = sqrtf(dx * dx + dy * dy);
      if (r < R_MIN) r = R_MIN;
      const int depth = (int)(DEPTH_K / r);
      const int ang = (int)((atan2f(dy, dx) + (float)M_PI) * ANG_SCALE);
      fld[y * W + x] = (uint8_t)((depth + ang) & fx::BANK_MASK);
    }
  }
  fx::writeRamp(f, WORMHOLE, 6, rotate);
}

void TunnelEffect::step(lgfx::LGFX_Sprite& f) {
  // Two counters and a block copy: that is the entire per-frame cost of a
  // tunnel. The field moves one way, the colors crawl the other, and the eye
  // reads the difference as banded light sliding over a moving surface.
  phase = (uint8_t)(phase + dir * FLOW);
  rotate = (uint8_t)((rotate - dir * CRAWL) & fx::BANK_MASK);
  fx::blit2x(f, phase);
  fx::writeRamp(f, WORMHOLE, 6, rotate);
}

#endif
