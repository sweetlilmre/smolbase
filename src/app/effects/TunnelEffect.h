// Tunnel — the palette wormhole. Every pixel's depth (1/r) and angle from the
// vanishing point are baked ONCE into two static coordinate planes; after that
// the endless rush down the pipe is two counters adding into those planes and
// a checkered texture read through them. Nothing is ever recomputed: the depth
// counter sucks the walls inward, the angle counter rotates them, and a light
// band crawling through the palette rolls over the top of both.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class TunnelEffect : public Effect {
  uint8_t uAcc = 0;   // depth shift: the fall down the pipe, one texel a frame
  uint8_t vAcc = 0;   // angle shift: the roll around it, one texel a frame
  uint16_t pulse = 0; // frame counter: the ramp's breath and the window's pan
  int8_t dir = 1;     // tap flips it: fly in, or fall out

  size_t scratchBytes() const override; // two coordinate planes plus the wall

  void buildTexture();
  void writePalette(lgfx::LGFX_Sprite& f);

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { dir = (int8_t)-dir; }
};

#endif
