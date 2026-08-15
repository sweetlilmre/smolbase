// Wormhole — the palette trick in its purest form, and a straight port of how
// the original demo did it: a 640x400 field of palette indices sits in flash
// and never changes, a 240x240 window scrolls around it, and the ramp rotates
// underneath. No geometry is computed at runtime and no heap is held — the
// frame is 240 row copies out of flash and 128 palette writes.
//
// Everything before this was a mistake worth remembering. The effect was built
// three times as runtime geometry — first distance-and-angle around a point,
// then a ray-cast cylinder, then a funnel with a rim — chasing a shape that
// was never computed in the first place. The original is a pre-rendered image.
// When the target is a fixed shape, the cheapest and most faithful thing to
// ship is the shape.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class TunnelEffect : public Effect {
  uint8_t rotate = 0;  // where the ramp currently sits: half the animation
  uint16_t frame = 0;  // drives the window's drift and the ramp's slow breath
  int8_t dir = 1;      // tap flips it: fall in, or climb out

  void writePalette(lgfx::LGFX_Sprite& f);

public:
  // Nothing. The field is in flash and the frame is written straight into the
  // framebuffer, so this effect is the one that costs no heap at all.
  size_t scratchBytes() const override { return 0; }

  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { dir = (int8_t)-dir; }
};

#endif
