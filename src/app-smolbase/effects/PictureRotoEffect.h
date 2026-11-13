// Picture rotozoomer — the other half of the 1993 trick. Where RotozoomEffect
// spins a PROCEDURAL texture under a cycling palette, this one spins a fixed
// PICTURE under a fixed palette, which is how the effect was used in practice:
// the tile is artwork, the palette never moves, and the motion is entirely in
// the transform. Both are worth having, so neither replaces the other.
//
// The texture must have a BLACK BORDER. The lookup wraps with a mask, so the
// tile repeats whenever the zoom is out far enough to show more than one of
// it, and a uniform dark edge makes that wrap invisible. A centred subject on
// a dark field therefore tiles perfectly without being a tiling pattern —
// which is the whole reason this effect can use a picture at all.
//
// The tile is generated in buildStandIn() -- a shaded orb with four satellites
// on a black field. It is a stand-in by name but it earns its place: smooth
// shading to show how the ramp bands, fine detail to show how the transform
// aliases when the zoom minifies. Swapping in real artwork means replacing one
// 128x128 field of indices and its palette; nothing else here would change.
#pragma once
#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class PictureRotoEffect : public Effect {
  // Rotation and zoom run on INDEPENDENT phases, unlike RotozoomEffect, where
  // one angle drives both and the zoom sweeps through zero. That coupling is
  // the procedural effect's signature; here it would be a defect, because a
  // picture magnified until one texel fills the screen is not a picture any
  // more. The two periods are deliberately not a small whole-number ratio, so
  // the pair does not visibly repeat.
  float angle = 0.0f;
  float zoomPhase = 0.0f;
  // The rotation's still point wanders instead of sitting at the screen centre
  // -- that, not a texture drift, is what makes the field sweep. See the long
  // note in the .cpp for the two drifts that were tried first and why a tiled
  // texture defeats both.
  float pivotPhaseX = 0.0f, pivotPhaseY = 0.0f;
  int8_t spin = 1; // tap reverses the rotation

  size_t scratchBytes() const override { return fx::TEX_W * fx::TEX_W; } // 16 KB

  void buildStandIn();
  void writePalette(lgfx::LGFX_Sprite& f);

public:
  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { spin = (int8_t)-spin; }
};

#endif
