// Fire — the other effect every 256-color demo shipped. A heat map, seeded hot
// along the bottom row and blurred upward one row per frame, read straight
// through a black-red-yellow-white ramp: the palette IS the flame, the pixels
// are just numbers. Runs half-res in the shared scratch (chunky was the look).
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class FireEffect : public Effect {
  uint8_t blast = 0; // tap: frames of extra fuel still to burn

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { blast = 12; }
};

#endif
