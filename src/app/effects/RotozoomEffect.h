// Rotozoomer — the 1992 party trick: a 64x64 texture rotated and scaled over
// the whole screen with no per-pixel maths beyond two fixed-point adds and a
// masked lookup. The rotation matrix is computed once per FRAME; the inner
// loop just walks texture space in a straight line and lets the mask wrap it.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class RotozoomEffect : public Effect {
  float angle = 0.0f, zoomPhase = 0.0f;
  int32_t driftU = 0, driftV = 0; // 16.16 texture-space pan
  uint8_t rotate = 0;
  uint8_t variant = 0; // tap swaps the texture
  bool rebuild = true;

  void buildTexture();

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override {
    variant ^= 1;
    rebuild = true;
  }
};

#endif
