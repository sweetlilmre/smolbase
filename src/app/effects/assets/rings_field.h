// The wormhole, straight out of Psycho Neurosis (Asphyxia, 1994) — part 003,
// scene 1. Both halves of the effect live here in flash, and neither is
// computed at runtime, because neither was computed in 1994 either:
//
//   RINGS_FIELD   the demo's own 640x400 image, one palette index per pixel,
//                 unresampled. 250 KB of .rodata and no heap at all.
//   RINGS_RED/    the three 225-byte channel tables, 6-bit VGA values exactly
//   GREEN/BLUE    as the DAC received them.
//
// The palette is 15 bands of 15, and it is not just colour — printed as a
// 15x15 grid of set/clear it draws a letter A framed in black. That glyph and
// the dark grid between the tunnel tiles are painted by the PALETTE, not by
// the artwork, which is why the field on its own looks like plain rings.
//
// Regenerated from the reconstruction at D:\source\psycho by
// scripts/make_rings_field.py. Provenance: docs/THIRD-PARTY.md.
#pragma once
#include <stdint.h>

constexpr int RINGS_W = 640;
constexpr int RINGS_H = 400;
// 225 colours starting at DAC index 1, as the original uploaded them.
constexpr int RINGS_COLOURS = 225;
constexpr int RINGS_BANDS = 15;
constexpr int RINGS_BAND_SIZE = 15;
constexpr uint8_t RINGS_FIRST_INDEX = 1;

extern const uint8_t RINGS_FIELD[RINGS_H * RINGS_W];
extern const uint8_t RINGS_RED[RINGS_COLOURS];
extern const uint8_t RINGS_GREEN[RINGS_COLOURS];
extern const uint8_t RINGS_BLUE[RINGS_COLOURS];
