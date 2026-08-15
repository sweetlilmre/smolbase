// The wormhole's index field: the demo's own 640x400 image, one palette index
// per pixel, living in flash (.rodata) — 250 KB there and not one byte of heap.
//
// This is the whole effect, and it is how the original worked: the shape was
// never computed at runtime, it was *drawn*. A 240x240 window scrolls around
// this image while the palette rotates underneath. The window is smaller than
// the field on purpose — that is where the drifting comes from, and because
// the field is in flash the drift is free, where the same trick over a RAM
// buffer cost 17 KB and made the device unflashable.
//
// Stored unresampled: every pixel is the demo's own. The values are a sawtooth
// that wraps at 128, so scaling or interpolating them would invent a ring
// across the wrap that was never there.
//
// Regenerate with: uv run scripts/make_rings_field.py
// Provenance and licensing: docs/THIRD-PARTY.md.
#pragma once
#include <stdint.h>

constexpr int RINGS_W = 640;
constexpr int RINGS_H = 400;
extern const uint8_t RINGS_FIELD[RINGS_H * RINGS_W];
