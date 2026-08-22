// Panel config lifted VERBATIM from src/core/Display.cpp (SmolPanel), with the
// smolbase_config.h pin macros inlined. Every value here is load-bearing and
// documented in docs/research/display-stack-selection.md — the point of check 3
// is whether this exact config still initialises the panel under IDF 6, so do
// not "improve" anything in this file.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// include/smolbase_config.h
#define SPIKE_PIN_SCLK 18
#define SPIKE_PIN_MOSI 23
#define SPIKE_PIN_DC 2
#define SPIKE_PIN_RST 4
#define SPIKE_PIN_BL 25    // active-LOW PWM
#define SPIKE_PIN_TOUCH 32 // capacitive pad, touch channel T9
#define SPIKE_SPI_HZ 40000000

class SpikePanel : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  SpikePanel() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 3; // CS tied to GND: ST7789 samples on a mode-3 clock
      cfg.freq_write = SPIKE_SPI_HZ;
      cfg.freq_read = 16000000; // unused (no MISO), harmless
      cfg.spi_3wire = true;     // write-only panel
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = SPIKE_PIN_SCLK;
      cfg.pin_mosi = SPIKE_PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = SPIKE_PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = -1; // hard-wired low
      cfg.pin_rst = SPIKE_PIN_RST;
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
      cfg.pin_bl = SPIKE_PIN_BL;
      cfg.invert = true; // active-LOW backlight
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
