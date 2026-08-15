// Tunnel — the palette wormhole. Every pixel's distance and angle from the
// center are baked ONCE into a static field of palette indices; after that the
// endless rush down the pipe is a single byte added to that field per frame,
// plus a palette that crawls the other way. No geometry is ever recomputed —
// the illusion of depth is entirely in the numbering and the colors.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class TunnelEffect : public Effect {
  uint8_t phase = 0;  // where the static field currently sits in the bank
  uint8_t rotate = 0; // where the ramp currently sits — the counter-crawl
  int8_t dir = 1;     // tap flips it: fly in, or fall out

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { dir = (int8_t)-dir; }
};

#endif
