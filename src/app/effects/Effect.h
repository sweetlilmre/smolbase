// The demo roster's plug-in point (issue #105). One Screen (DemoScreen) owns the
// panel; the full-screen 256-color effect running behind the clock is swapped by
// long press. Each effect is one class in one file under src/app/effects/ and
// touches nothing but the framebuffer's pixels and its own slice of the palette.
//
// The contract every effect signs:
//
//   - It owns the EFFECT BANK (palette indices 0x00-0x7F) and nothing else,
//     except the wormhole, which replays a whole original palette of 225
//     colours from index 1. Either way the screen's own entries sit at
//     0xF0-0xF5, above anything an effect claims, so the clock overlay stays
//     exactly the colors the user picked. Both halves are rewritten on every
//     switch, so an effect inherits no palette state and leaves none behind.
//   - enter() rebuilds everything it needs: only one effect runs at a time and
//     they SHARE one scratch buffer (below), so nothing survives an exit.
//   - step() paints the whole 240x240 index field. It is called at a fixed
//     30 Hz and must land well inside ~8 ms: present() spends ~24 ms of the
//     33 ms frame pushing pixels over SPI (docs/building-your-app.md), and the
//     clock overlay takes ~2 ms of what is left. No allocation, no blocking.
//   - Never draw text or read the clock — the screen draws the identity overlay
//     on top of every frame, after step() returns.
#pragma once
#include "../../core/App.h"
#include "../../core/Display.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

namespace fx {

// ---- The palette split -------------------------------------------------------
// 0x00-0x7F  the effect bank: 128 entries the running effect owns outright.
//            128 is a mask, not a modulo — `(v + phase) & BANK_MASK` is the
//            palette-cycling primitive these effects are built on, one
//            instruction per pixel, and a 128-entry ramp was a period-correct
//            budget on VGA too (the other 128 held the UI and the sprites).
// 0xF0       black — the drop shadow under every identity string.
// 0xF1-0xF5  the five identity colors, written verbatim from the settings. The
//            old screen quantized text through color332(); with its own bank
//            the overlay now shows the exact RGB the user picked.
// Everything else keeps the framebuffer's documented RGB332 identity, and the
// gap from 0x80 to 0xEF is deliberate: the wormhole replays the original
// demo's palette, which is 225 colours starting at index 1, so the overlay has
// to live above that range or the clock would be repainted by a tunnel.
constexpr uint8_t BANK_SIZE = 128;
constexpr uint8_t BANK_MASK = 0x7F;
constexpr uint8_t UI_BLACK = 0xF0;
constexpr uint8_t UI_TEXT = 0xF1; // .. 0xF5, one per identity string
constexpr uint8_t UI_LAST = 0xF5;

// ---- The Scratch -------------------------------------------------------------
// One byte pool, lent to whichever effect is running, sized to that effect and
// freed the moment nothing needs it. It is heap, not .bss: a static buffer does
// not fit — the DRAM segment already carries the 57.6 KB framebuffer, and the
// linker turned down even a single quarter-frame by ~4.4 KB.
//
// Each effect declares how many bytes it wants through Effect::scratchBytes()
// and carves its own layout out of them; an effect that needs none (the plasma
// computes everything from tiny per-frame tables) allocates nothing at all.
// The screen sizes the pool to the running effect and frees it whenever
// nothing needs it.
//
// SIZE IS A SAFETY PROPERTY HERE, not a preference. An OTA streams a ~1.2 MB
// image through the web server and needs free heap to do it. Measured on
// device: with ~77 KB free an upload succeeds, with ~60 KB it fails at the
// first parse, every time — and this board has no serial port to recover
// through. An earlier effect held 61 KB and made the device unflashable while
// it ran. Measured worst case now is the rotozoomer's 128x128 texture at
// 17.4 KB, leaving ~105 KB free. Do not grow it without re-testing an OTA with
// that effect running.
//
// Effects that work at half resolution here and expand through blit2x() are
// not conceding anything to the ESP32 — that is how these effects ran in 1993,
// on a 320x200 mode-13h screen a CRT stretched to fill a 640x400 tube. Chunky
// is the period-correct look.
constexpr int SCRATCH_W = 120;
constexpr int SCRATCH_H = 120;
constexpr int PLANE = SCRATCH_W * SCRATCH_H; // one half-res quarter-frame
constexpr int TEX_W = 128; // the rotozoomer's texture
constexpr int TEX_MASK = TEX_W - 1;
constexpr int TEX_SHIFT = 7; // log2(TEX_W): the row stride as a shift

// Size the pool to exactly what the incoming effect asked for (freeing and
// re-allocating if that differs), or hand it back entirely. Returns nullptr if
// the allocation failed, in which case the screen falls back to the calm clock.
uint8_t* ensureScratch(size_t bytes);
uint8_t* scratch(); // the current pool; nullptr when nothing holds one
void releaseScratch();

// Expand plane 0 into the full frame as 2x2 chunky pixels. ~1 ms.
void blit2x(lgfx::LGFX_Sprite& f);

// ---- Palette ramps -----------------------------------------------------------
// An effect declares its colors as a handful of stops and lets writeRamp()
// interpolate the 128 entries between them. `rotate` shifts the whole ramp
// around the bank: incrementing it once per frame IS the palette cycling, the
// trick this entire roster is built to show off — 128 three-byte writes,
// applied at the next present(), no pixel touched.
struct Stop {
  uint8_t at; // bank index this color lands on (0..127, ascending, first = 0)
  uint8_t r, g, b;
};
void writeRamp(lgfx::LGFX_Sprite& f, const Stop* stops, int count, uint8_t rotate);

// The colour a ramp takes at one bank index, for callers that need the value
// rather than the write — blending between two ramps, mostly.
struct Rgb888 {
  uint8_t r, g, b;
};
Rgb888 rampColor(const Stop* stops, int count, int i);

// xorshift PRNG — the fire needs one per pixel, which rules out random().
uint8_t rand8();

} // namespace fx

// One switchable effect. Long press moves to the next; tap is the effect's own
// knob (kick the ball, poke the fire, reverse the tunnel).
class Effect {
public:
  virtual ~Effect() = default;
  // How much scratch this effect wants, and how it carves it up, is entirely
  // the effect's business — the screen only allocates it. Zero means none.
  virtual size_t scratchBytes() const { return 0; }
  virtual void enter(lgfx::LGFX_Sprite& f) = 0; // claim the bank, fill the scratch
  virtual void step(lgfx::LGFX_Sprite& f) = 0;  // one 30 Hz frame, full screen
  virtual void onTap() {}
};

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
