// Plasma — the effect every 256-color demo opened with, in three flavours the
// tap cycles between. All three read a summed field through a rotating ramp;
// what differs is where the field comes from.
//
//   0 CLASSIC     separable sines of x, y and x+y. Cheap — one lookup per axis
//                 hoisted out of the pixel loop — but separable means the
//                 pattern is a fixed lattice that can only slide past itself.
//   1 RIPPLES     three moving centres, each contributing a circular wave.
//                 Nothing is separable, so as the centres orbit the
//                 interference genuinely deforms rather than translating. The
//                 circles cost no sqrt: a table indexed by r-SQUARED holds the
//                 sine of the root (technique: 4rknova.com/blog/2016/11/01/plasma).
//   2 INTERFERE   a circular wave MULTIPLIED by a diagonal one. Multiplying
//                 rather than adding pinches the bands into sharp nodes that
//                 sweep as the phases beat against each other.
//
// The ramp runs on its own 30-second clock across all three, crossfading
// rather than snapping, so the field and the colours never change together.
//
// Flavours 1 and 2 run at half resolution and expand 2x, which buys the extra
// per-pixel work back; the chunk is period-correct anyway.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class PlasmaEffect : public Effect {
  uint8_t sinTab[256];  // one shared sine, scaled so the terms fill the bank
  uint8_t colVal[240];  // per-frame horizontal term (classic)
  uint8_t rowVal[240];  // per-frame vertical term (classic)
  uint8_t diagVal[480]; // per-frame diagonal term, indexed [x + y] (classic)
  uint16_t t1 = 0, t2 = 0, t3 = 0;
  uint16_t frame = 0;
  uint8_t rotate = 0;
  uint8_t flavour = 0;
  uint8_t rampIdx = 0;    // which ramp; advances on its own 30 s clock
  uint16_t rampHold = 0;  // frames spent on it, fade included

  void writeRamp(lgfx::LGFX_Sprite& f);
  void buildRingTable();
  void placeSources();
  void classic(lgfx::LGFX_Sprite& f);
  void ripples(lgfx::LGFX_Sprite& f);
  void interfere(lgfx::LGFX_Sprite& f);

public:
  // A ring table plus per-source squared-distance rows. The classic flavour
  // uses none of it, but the pool is sized on switch-in, not per frame.
  size_t scratchBytes() const override;

  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { flavour = (uint8_t)((flavour + 1) % 3); }
};

#endif
