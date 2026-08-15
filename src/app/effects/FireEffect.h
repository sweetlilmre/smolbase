// Fire — the other effect every 256-color demo shipped. A heat map, seeded hot
// along the bottom row and blurred upward one row per simulation step, read
// straight through a black-red-yellow-white ramp: the palette IS the flame, the
// pixels are just numbers. Runs half-res in the shared scratch (chunky was the
// look) and at half the frame rate (30 Hz fire races; real fire loafs).
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class FireEffect : public Effect {
  // The fuel row persists and drifts. Re-rolling it from scratch every frame
  // was the mistake in the first cut: it made the base of the flame boil at
  // 30 Hz, which reads as television static, not fire.
  uint8_t ember[fx::SCRATCH_W];
  uint8_t phase = 0; // simulate on every other frame
  uint8_t blast = 0; // tap: simulation steps of extra fuel still to burn

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { blast = 6; }
};

#endif
