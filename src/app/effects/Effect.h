// The demo roster's plug-in point (issue #105). One Screen (DemoScreen) owns the
// panel; the full-screen 256-color effect running behind the clock is swapped by
// long press. Each effect is one class in one file under src/app/effects/ and
// touches nothing but the framebuffer's pixels and its own slice of the palette.
//
// The contract every effect signs:
//
//   - It owns the EFFECT BANK (palette indices 0x00-0x7F) and nothing else. The
//     screen owns 0x80-0x85 (black + the five identity text colors), so the
//     clock overlay stays exactly the colors the user picked no matter what the
//     effect does to its own 128 entries. Both halves are rewritten on every
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
// 0x80       black — the drop shadow under every identity string.
// 0x81-0x85  the five identity colors, written verbatim from the settings. The
//            old screen quantized text through color332(); with its own bank
//            the overlay now shows the exact RGB the user picked.
// 0x86-0xFF  untouched: still the framebuffer's documented RGB332 identity.
constexpr uint8_t BANK_SIZE = 128;
constexpr uint8_t BANK_MASK = 0x7F;
constexpr uint8_t UI_BLACK = 0x80;
constexpr uint8_t UI_TEXT = 0x81; // .. 0x85, one per identity string
constexpr uint8_t UI_LAST = 0x85;

// ---- The Scratch -------------------------------------------------------------
// One byte pool, lent to whichever effect is running. It is the ONE piece of
// memory the roster shares, so six effects cost what the hungriest one costs:
// a single heap allocation at the first switch-in, never freed (a static
// buffer does not fit — see Effect.cpp). scratchReady() is false only if that
// allocation failed, in which case the screen falls back to the calm clock.
//
// Layout — planes are 120x120, a quarter-frame each:
//   plane 0   ball pixels / fire's heat map / the tunnel's depth field
//   plane 1   the tunnel's angle field
//   TEX_OFF   a texture: 128x128 for the tunnel's wall, 64x64 for the
//             rotozoomer (which uses the front quarter of the same space)
// The tunnel is what sizes the pool: a textured tunnel needs BOTH coordinates
// per pixel, and collapsing them into one byte (which is what a 14.4 KB pool
// would force) can only produce a spiral gradient, never a checkered funnel.
//
// Effects that work at half resolution here and expand through blit2x() are
// not conceding anything to the ESP32 — that is how these effects ran in 1993,
// on a 320x200 mode-13h screen a CRT stretched to fill a 640x400 tube. Chunky
// is the period-correct look.
constexpr int SCRATCH_W = 120;
constexpr int SCRATCH_H = 120;
constexpr int PLANE = SCRATCH_W * SCRATCH_H;
constexpr int TEX_W = 64;  // the rotozoomer's texture
constexpr int TEX_MASK = TEX_W - 1;
constexpr int TEX_OFF = 2 * PLANE;
constexpr int TEX_BYTES = 128 * 128; // the tunnel's wall, which sizes the tail
constexpr int SCRATCH_BYTES = TEX_OFF + TEX_BYTES;
uint8_t* scratch();
bool scratchReady();

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

// xorshift PRNG — the fire needs one per pixel, which rules out random().
uint8_t rand8();

} // namespace fx

// One switchable effect. Long press moves to the next; tap is the effect's own
// knob (kick the ball, poke the fire, reverse the tunnel).
class Effect {
public:
  virtual ~Effect() = default;
  virtual void enter(lgfx::LGFX_Sprite& f) = 0; // claim the bank, fill the scratch
  virtual void step(lgfx::LGFX_Sprite& f) = 0;  // one 30 Hz frame, full screen
  virtual void onTap() {}
};

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
