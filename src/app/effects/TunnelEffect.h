// Wormhole — Psycho Neurosis (Asphyxia, 1994), part 003 scene 1, replayed.
//
// There is no geometry here and there never was in the original either. A
// 640x400 image of palette indices sits in flash; a 240x240 window spirals
// around it; and the 225-entry palette rotates twice per step — each 15-colour
// band left by one, then the whole table right by one whole band. Those two
// rotations against each other are what make the rings flow inward. The letter
// A on the tunnel wall is in the palette, not the artwork.
//
// Everything before this was a mistake worth remembering: the effect was built
// three times as runtime geometry — distance-and-angle, a ray-cast cylinder,
// then a funnel with a rim — chasing a shape that was never computed in the
// first place. When the target is a fixed image, ship the image.
#pragma once
#include "Effect.h"
#include "assets/rings_field.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class TunnelEffect : public Effect {
  // Working copies of the three channel tables. The original's were typed
  // constants — writable, so the rotations shuffled them in place; ours are in
  // flash, so these 675 bytes are the only RAM the effect touches.
  uint8_t red[RINGS_COLOURS];
  uint8_t green[RINGS_COLOURS];
  uint8_t blue[RINGS_COLOURS];

  float angle = 0.0f, radius = 0.0f;
  bool inward = true;  // the original ended at the centre; a clock face cannot
  uint16_t rotAcc = 0; // 70 Hz of palette rotation stepped out over 30 Hz frames
  int8_t dir = 1;      // tap runs the rotations backwards: the rings climb out

  void rotateFine(uint8_t* t);
  void rotateCoarse(uint8_t* t);
  void uploadPalette(lgfx::LGFX_Sprite& f);

public:
  // None. The field and the palette are both in flash and the frame is written
  // straight into the framebuffer, so this effect holds no heap at all.
  size_t scratchBytes() const override { return 0; }

  void enter(lgfx::LGFX_Sprite& f) override;
  void step(lgfx::LGFX_Sprite& f) override;
  void onTap() override { dir = (int8_t)-dir; }
};

#endif
