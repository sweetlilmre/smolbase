#include "Effect.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include <cstdlib>
#include <cstring>

namespace fx {

namespace {
uint8_t* scratchData = nullptr;
size_t scratchSize = 0;
uint32_t rngState = 0x1234abcdu;
} // namespace

// Heap, not .bss: a static buffer was tried and does not fit — the DRAM
// segment already carries the 57.6 KB framebuffer, and the linker turned down
// even a single quarter-frame by ~4.4 KB. Sized to the running effect and
// released the moment nothing needs it, because free heap is what an OTA
// spends (see the header).
uint8_t* scratch() { return scratchData; }

uint8_t* ensureScratch(size_t bytes) {
  if (scratchData && scratchSize == bytes) return scratchData;
  releaseScratch();
  scratchData = (uint8_t*)malloc(bytes);
  scratchSize = scratchData ? bytes : 0;
  return scratchData;
}

void releaseScratch() {
  free(scratchData);
  scratchData = nullptr;
  scratchSize = 0;
}

uint8_t rand8() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return (uint8_t)(rngState >> 16);
}

// Row-doubling by memcpy: the expensive row is computed once, the twin is a
// 240-byte block copy. Raw buffer access — the frame is a contiguous 240x240
// byte plane in both 8-bpp modes.
void blit2x(lgfx::LGFX_Sprite& f) {
  uint8_t* fb = (uint8_t*)f.getBuffer();
  const uint8_t* src = scratchData;
  for (int y = 0; y < SCRATCH_H; ++y) {
    uint8_t* dst = fb + (y * 2) * 240;
    for (int x = 0; x < SCRATCH_W; ++x) {
      const uint8_t v = (uint8_t)(src[x] & BANK_MASK);
      dst[x * 2] = v;
      dst[x * 2 + 1] = v;
    }
    memcpy(dst + 240, dst, 240);
    src += SCRATCH_W;
  }
}

Rgb888 rampColor(const Stop* stops, int count, int i) {
  int seg = 0;
  while (seg + 1 < count - 1 && i >= stops[seg + 1].at) ++seg;
  const Stop& a = stops[seg];
  const Stop& b = stops[seg + 1];
  const int span = (b.at > a.at) ? (b.at - a.at) : 1;
  const int t = (i < a.at) ? 0 : ((i > b.at) ? span : (i - a.at));
  return {(uint8_t)(a.r + (b.r - a.r) * t / span),
          (uint8_t)(a.g + (b.g - a.g) * t / span),
          (uint8_t)(a.b + (b.b - a.b) * t / span)};
}

void writeRamp(lgfx::LGFX_Sprite& f, const Stop* stops, int count, uint8_t rotate) {
  for (int i = 0; i < BANK_SIZE; ++i) {
    const Rgb888 c = rampColor(stops, count, i);
    f.setPaletteColor((uint8_t)((i + rotate) & BANK_MASK), c.r, c.g, c.b);
  }
}

} // namespace fx

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
