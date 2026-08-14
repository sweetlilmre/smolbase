#include "Display.h"
#include "smolbase_config.h"
#include <Arduino.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace {

// Known-good panel config for the Small TV Pro's ST7789V 240x240 — every value here
// is load-bearing and documented in docs/research/display-stack-selection.md.
class SmolPanel : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  SmolPanel() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 3; // CS tied to GND: ST7789 samples on a mode-3 clock
      cfg.freq_write = SMOLBASE_SPI_HZ;
      cfg.freq_read = 16000000; // unused (no MISO), harmless
      cfg.spi_3wire = true;     // write-only panel
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = SMOLBASE_PIN_SCLK;
      cfg.pin_mosi = SMOLBASE_PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = SMOLBASE_PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = -1; // hard-wired low
      cfg.pin_rst = SMOLBASE_PIN_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.memory_width = 240;
      cfg.memory_height = 320; // GRAM is 240x320; keeps rotation offsets automatic
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true; // IPS ST7789V
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = SMOLBASE_PIN_BL;
      cfg.invert = true; // active-LOW backlight
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

SmolPanel panel;

#if SMOLBASE_FRAMEBUFFER != SMOLBASE_FB_NONE
uint8_t fbData[240 * 240]; // static, .bss — heap stays contiguous for TLS
lgfx::LGFX_Sprite fbSprite(&panel);
#endif

Screen* userScreen = nullptr;
Screen* overrideScreen = nullptr;

Screen* owner() { return overrideScreen ? overrideScreen : userScreen; }

void enter(Screen* s) {
  if (s) s->onEnter(panel);
}

} // namespace

namespace Display {

lgfx::LGFX_Device& gfx() { return panel; }

void begin() {
  panel.init();
  panel.setBrightness(200);
  panel.fillScreen(TFT_BLACK);
#if SMOLBASE_FRAMEBUFFER != SMOLBASE_FB_NONE
  fbSprite.setColorDepth(8);
  fbSprite.setBuffer(fbData, 240, 240, 8);
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
  // Palette storage is a one-time 1 KB boot allocation inside LovyanGFX (there is no
  // static-palette API); the 57.6 KB pixel buffer above stays static. Default palette:
  // index i decodes as RGB332 (RRRGGGBB), so color332(r,g,b) picks a usable index and
  // the full color cube is roughly covered. See Display.h for how to override entries.
  fbSprite.createPalette();
  for (int i = 0; i < 256; ++i) {
    uint8_t r = ((i >> 5) & 0x07) * 255 / 7;
    uint8_t g = ((i >> 2) & 0x07) * 255 / 7;
    uint8_t b = (i & 0x03) * 255 / 3;
    fbSprite.setPaletteColor(i, r, g, b);
  }
#endif
  // RGB332 mode: no palette — a true-color 8-bpp sprite; every draw call's
  // color converts through LovyanGFX's normal RGB332 quantization (#102).
  fbSprite.fillScreen(0); // black in both modes (palette index 0 / rgb332 0)
#endif
}

#if SMOLBASE_FRAMEBUFFER != SMOLBASE_FB_NONE
lgfx::LGFX_Sprite& frame() { return fbSprite; }

void present() { fbSprite.pushSprite(0, 0); }

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_RGB332
void present(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > 240) h = 240 - y;
  if (h <= 0) return;
  panel.pushImage(0, y, 240, h, (const lgfx::rgb332_t*)(fbData + y * 240));
}
#endif
#endif

void setActive(Screen* s) {
  if (userScreen == s) return;
  if (!overrideScreen && userScreen) userScreen->onExit();
  userScreen = s;
  if (!overrideScreen) enter(userScreen);
}

void systemTakeover(Screen* s) {
  if (overrideScreen == s) return;
  Screen* prev = owner();
  if (prev) prev->onExit();
  overrideScreen = s;
  enter(overrideScreen);
}

void systemRelease() {
  if (!overrideScreen) return;
  overrideScreen->onExit();
  overrideScreen = nullptr;
  enter(userScreen);
}

Screen* active() { return owner(); }

void tick() {
  Screen* s = owner();
  if (s) s->tick(panel);
}

void setBrightness(uint8_t level) { panel.setBrightness(level); }

} // namespace Display
