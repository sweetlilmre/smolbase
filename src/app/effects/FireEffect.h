// Fire — the firedemo effect by Javier "Jare" Arevalo of Iguana (1993), the
// one that started the genre. Rebuilt from the author's own HTML5 port
// (https://github.com/TheJare/FiredemoHTML5) rather than from the textbook
// version everyone else copied. The algorithm and the palette are his, used
// under the MIT licence: (C) 2013 by Javier Arevalo. Notice and full text in
// docs/licenses/MIT-firedemo.txt; see also docs/THIRD-PARTY.md.
//
// It differs from the usual fire in every particular, and the differences are
// the whole look:
//   - the blur is all EIGHT neighbours, symmetric, not three-below-plus-one;
//   - nothing rises through the kernel — the buffer is SCROLLED up one row a
//     frame, and that is the flame's speed;
//   - the cooling is not random. A quarter of cells are picked by the low bits
//     of the neighbour sum, and cooled by one WRAPPING at zero, so a cold cell
//     near the base becomes a white-hot spark. Jare has said the effect came
//     out of "a few programming mistakes"; that wrap is the best of them, and
//     it is where every spark in the fire comes from.
// The buffer is tiny (60x60, expanded 4x) because his was: the softness comes
// from magnifying a small heavily-blurred field, not from smoothing a big one.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class FireEffect : public Effect {
  uint8_t blast = 0; // tap: frames of extra fuel shoved into the base

public:
  // Two buffers — the eight-neighbour kernel reads the row above, so this one
  // cannot run in place. One row of slack: the render reads a row ahead.
  size_t scratchBytes() const override;

  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { blast = 8; }
};

#endif
