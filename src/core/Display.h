// LovyanGFX device (ticket #3 known-good config) + the Screen slot (ticket #6):
// one active Screen; the system may take the slot over (AP mode) and restores the
// consumer's screen afterwards.
#pragma once
#include "App.h"
#include "smolbase_config.h"

namespace Display {
void begin();
lgfx::LGFX_Device& gfx();

#if SMOLBASE_FRAMEBUFFER != SMOLBASE_FB_NONE
// Optional full-frame composition sprite (opt-in). Draw into frame(), then call
// present() to push it to the panel in one DMA-backed transfer. Screens keep drawing
// direct via tick(LGFX_Device&) — the sprite is a composition tool, never a
// requirement. The pixel buffer is static (.bss). There is no full-frame RGB565 mode
// (115.2 KB doesn't fit this chip); for full-color composition, create a partial-frame
// 16-bpp lgfx::LGFX_Sprite of your own and push it where needed.
//
// frame() is an 8-bpp sprite in one of two modes (see smolbase_config.h):
// PALETTE_8 — indexed; color arguments are RAW palette indices. The default
// palette maps index i as RGB332 (bits RRRGGGBB), so lgfx::color332(r, g, b)
// yields a sensible index, 0x00 is black and 0xFF is white. Customize with
//   Display::frame().setPaletteColor(index, r, g, b); // each component 0-255
// (changes land at the next present()). Enables palette-cycling animation,
// but COMPUTED colors (anti-aliased text, pushImage conversion) write garbage.
// RGB332 (#102) — true-color; every draw converts correctly (AA text, RGB565
// images), no palette tricks. lgfx::color332(r, g, b) is the color value.
lgfx::LGFX_Sprite& frame();
void present();
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_RGB332
// Push only rows [y, y+h) of the frame (full width) — a band redraw lands on
// the panel in one short write (~2 ms for 20 rows vs ~23 ms full frame), so a
// moving element can update at 30 Hz without full-frame tearing (#102).
// RGB332 mode only: the raw buffer is true-color, no palette lookup needed.
void present(int y, int h);
#endif
#endif

void setActive(Screen* s);         // consumer-facing slot
void systemTakeover(Screen* s);    // core only: AP-info screen etc.
void systemRelease();              // restore the consumer's screen
Screen* active();                  // whoever owns the panel right now

void tick();                       // main loop: dispatch to the owning screen
void setBrightness(uint8_t level); // 0-255
} // namespace Display
