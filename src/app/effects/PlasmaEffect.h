// Plasma — the effect every 256-color demo opened with. Four sine fields summed
// per pixel, the sum read as a palette index, the palette itself rotating: the
// colors move faster than the geometry and the eye reads liquid. Technique
// notes: docs/research/palette-effects.md.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class PlasmaEffect : public Effect {
  uint8_t sinTab[256];  // one shared sine, scaled so three terms fill the bank
  uint8_t colVal[240];  // per-frame horizontal term
  uint8_t rowVal[240];  // per-frame vertical term
  uint8_t diagVal[480]; // per-frame diagonal term, indexed [x + y]
  uint16_t t1 = 0, t2 = 0, t3 = 0;
  uint8_t rotate = 0;
  uint8_t scheme = 0; // tap cycles the ramp: rainbow / ember / ice

  void writeScheme(lgfx::LGFX_Sprite& f);

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { scheme = (uint8_t)((scheme + 1) % 3); }
};

#endif
