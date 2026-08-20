// The Amiga Boing Ball (1984, Dale Luck & R.J. Mical) — the effect this roster
// grew out of, and the oldest palette trick in it: the ball never rotates as
// pixels, 14 cycling palette entries do the work. Technique details:
// docs/research/boing-ball-technique.md; geometry and tuning live in the .cpp.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class BoingEffect : public Effect {
  int cyclePhase = 0;
  float bx = 120, by = 90, vx = 2.2f, vy = 0;
  bool spriteReady = false; // the sprite's one-time palette allocation, not the buffer
  // The pre-rendered ball, drawn into the shared 120x120 scratch — which is
  // exactly a ball-sized buffer, and the reason the roster needs no memory
  // beyond the one it already had (this used to be the app's single 14.4 KB
  // heap allocation). Its palette contents never matter: palette-8 ->
  // palette-8 pushes copy raw indices, and the frame's palette decides the
  // colors (research #45).
  lgfx::LGFX_Sprite ball;

  size_t scratchBytes() const override { return fx::PLANE; } // the pre-rendered ball

  void applyCycle(lgfx::LGFX_Sprite& f);
  void stepPhysics();
  void drawShadow(lgfx::LGFX_Sprite& f, int cx, int cy);

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  // Tap kicks the ball along its direction of travel; the boost decays back to
  // the natural bounce over the next few floor contacts (#60, #100, #101).
  void onTap() override;
};

#endif
