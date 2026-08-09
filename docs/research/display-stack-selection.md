# Display Stack Selection

**Ticket:** wayfinder/tickets/0002-display-stack-selection.md
**Date:** 2026-08-08
**Target:** ST7789V 240x240 IPS over SPI on classic ESP32 (ESP32-D0WD, no PSRAM, 8MB flash), arduino-esp32 3.x / ESP-IDF 5.x, PlatformIO + pioarduino.
**Pins:** SCLK 18, MOSI 23, DC 2, RST 4, CS tied to GND, backlight GPIO25 (active-LOW PWM).

## Recommendation: LovyanGFX

Use **LovyanGFX** (`lovyan03/LovyanGFX@^1.2.26`) as smolbase's display stack. It is the only candidate that simultaneously delivers: working SPI DMA on the *classic* ESP32 under IDF 5.x, active maintenance with explicit arduino-esp32 3.x / IDF 5+ fixes in its release history, static-buffer-friendly sprites for a no-PSRAM device, and first-class handling of this panel's two quirks (CS tied low, 240x240-on-320-line-controller rotation offsets) through configuration alone — no library-file patching.

```ini
; platformio.ini
lib_deps = lovyan03/LovyanGFX@^1.2.26
```

## Scorecard

| Criterion | LovyanGFX | Arduino_GFX | TFT_eSPI |
|---|---|---|---|
| Maintenance (as of 2026-08) | Active: pushed 2026-08-07; releases 1.2.24/1.2.25/1.2.26 in Jun/Jul 2026 | Active: pushed 2026-08-06; v1.6.7 released 2026-07-18 | **Stale**: last release V2.5.43 on 2024-03-06; 351 open issues |
| arduino-esp32 3.x / IDF 5.x | Yes — release notes contain explicit IDF5/core-3.x fixes (e.g. v1.1.16 "fix Light_PWM backlight not works under arduino-esp32 v3.0 (IDF 5)"), IDF 5.3 and IDF v6-prep fixes in later releases | Yes — core library builds on 3.x (only its `ESP32LCD8/16`/`ESP32RGBPanel` parallel buses remain 2.x-only, per README) | **Broken/regressed** — issues #3329, #3355 document core >2.0.14 / 3.0.0 breakage; README's own fix claims target S3 + Arduino 3.3.6, not classic ESP32 |
| DMA on classic ESP32 | **Yes** — `Bus_SPI.cpp` implements classic-ESP32 DMA (channel via `DPORT_SPI_DMA_CHAN_SEL_REG`, `spicommon_dmaworkaround_idle` quirk handling, `SPI_DMA_CH_AUTO` on IDF >=4.3, IDF5-guarded code paths), plus a DMA queue (`addDMAQueue`/`execDMAQueue`) | Yes — `Arduino_ESP32SPIDMA` databus explicitly compiles for `CONFIG_IDF_TARGET_ESP32`, `SPI_DMA_CH_AUTO`, 1024-px chunks | ESP32-classic DMA path exists but README states "DMA only on ESP32 S3 at the moment" for current board packages; classic-ESP32 DMA under IDF5 is unresolved |
| No-PSRAM friendliness | `LGFX_Sprite::setBuffer(void* buffer, w, h, bpp)` accepts a caller-owned (static) buffer; `setPsram(false)` forces internal RAM; 1/2/4/8-bit sprites with palette cut RAM 2-16x | `Arduino_Canvas` + reduced-depth canvas variants; heap-allocated | Sprites heap-allocated; 8-bit sprite support exists |
| API ergonomics | Adafruit_GFX-compatible surface + `print`/`drawString`, full primitive set, sprites with `pushSprite`/`pushRotateZoom`, config-object init (no library-header editing) | Similar drawing API; init via constructor composition (bus + display objects) | Good API, but panel selection requires editing `User_Setup.h` inside the library (or build flags) — hostile for a template repo consumed downstream |
| ST7789 240x240 quirks | Config-only: `pin_cs = -1` + `spi_mode = 3` for CS-grounded panels; `Panel_ST7789` defaults `memory_height = 320`, so with `panel_height = 240` the 80-px offset at rotations 2/3 is applied automatically by the `Panel_LCD` base | Supported (`ST7789 240x240` listed) via constructor offsets; mode-3 handled by bus config | Supported via `Setup24_ST7789` style defines; CS-low requires `TFT_SPI_MODE SPI_MODE3` define |

## Why not the others

### TFT_eSPI — rejected (maintenance + IDF5)
The most popular library (4.9k stars) but effectively unmaintained: last release **V2.5.43, 2024-03-06** (GitHub releases API), ~2.5 years before this writing, with 351 open issues. Its own issue tracker documents that Arduino board packages newer than 2.0.14 broke it (issue [#3329](https://github.com/Bodmer/TFT_eSPI/issues/3329) "library not working with espressif32 esp32-s3 Arduino core > 2.0.14", issue [#3355](https://github.com/Bodmer/TFT_eSPI/issues/3355) "Arduino-ESP32 version 3.0.0 does not compile"). The README's later compatibility claim ("works with ESP-IDF versions greater than 2.0.14, tested with the Arduino 3.3.6 board package") is scoped to **ESP32-S3 DMA**; the README also states "DMA only on ESP32 S3 at the moment". That is exactly the failure mode the ticket flagged: on a classic ESP32 under IDF 5.x you get no working DMA, on a stale codebase. The `User_Setup.h`-editing configuration model is also a poor fit for a template repo (consumers must patch a vendored library or maintain build-flag soup).

### Arduino_GFX (moononournation) — solid runner-up
Genuinely active (v1.6.7 on 2026-07-18, pushed 2026-08-06, 1.1k stars) and its `Arduino_ESP32SPIDMA` databus explicitly supports `CONFIG_IDF_TARGET_ESP32` with `SPI_DMA_CH_AUTO`. It would work here. It loses to LovyanGFX on: sprite/off-screen ergonomics (no user-supplied static buffer equivalent to `setBuffer`), throughput ceiling (its DMA bus pushes fixed 1024-pixel chunks vs LovyanGFX's queued whole-transfer DMA and register-level classic-ESP32 path), and its note that several of its ESP32 parallel buses were dropped for core 3.x, indicating thinner 3.x coverage at the bus layer. Keep it as the documented fallback if LovyanGFX ever stalls.

### bb_spi_lcd (bitbank2) — noted, not selected
Actively maintained (pushed 2026-05-23) and DMA-capable, but small traction (233 stars), a thinner drawing/sprite API, and far less ST7789-variant coverage. Not competitive for a template whose consumers expect a rich GFX surface.

### LVGL — pre-rejected by the ticket
Out of scope per ticket 0002: full widget toolkit + theme engine is heavyweight for a no-PSRAM template; smolbase only needs text, primitives, and off-screen composition.

## Known-good init for this exact panel

LovyanGFX is configured entirely in user code (nothing edited inside the library — critical for a template repo):

```cpp
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class SmolDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  SmolDisplay() {
    { // SPI bus
      auto cfg = _bus.config();
      cfg.spi_host    = VSPI_HOST;      // SPI3; frees HSPI for other peripherals
      cfg.spi_mode    = 3;              // REQUIRED: CS is tied to GND, so the ST7789
                                        // samples on a mode-3 clock (SCK idles high)
      cfg.freq_write  = 60000000;       // see clock note below
      cfg.freq_read   = 16000000;       // unused (no MISO) but harmless
      cfg.spi_3wire   = true;           // write-only panel, no MISO wired
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO; // classic-ESP32 DMA, auto channel (IDF >= 4.3)
      cfg.pin_sclk    = 18;
      cfg.pin_mosi    = 23;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // Panel
      auto cfg = _panel.config();
      cfg.pin_cs           = -1;   // CS hard-wired low
      cfg.pin_rst          = 4;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 240;
      cfg.memory_width     = 240;
      cfg.memory_height    = 320;  // ST7789 GRAM is 240x320; leaving this at the
                                   // Panel_ST7789 default makes LovyanGFX apply the
                                   // 80-px window offset automatically at rotations 2/3
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;    // adjust 0-3 if "up" is wrong for the enclosure
      cfg.readable         = false;
      cfg.invert           = true; // IPS ST7789V: colors are inverted without INVON
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false; // display has the SPI bus to itself
      _panel.config(cfg);
    }
    { // Backlight
      auto cfg = _light.config();
      cfg.pin_bl      = 25;
      cfg.invert      = true;      // REQUIRED: backlight is active-LOW
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

SmolDisplay display;

void setup() {
  display.init();               // also runs Light_PWM init (IDF5-safe since v1.1.16)
  display.setBrightness(128);   // 0-255, inversion handled by cfg.invert
}
```

### Notes and caveats

- **SPI clock reality check.** The classic ESP32 SPI peripheral derives its clock from 80 MHz with integer-ish dividers; exactly 60 MHz is not achievable and the request will resolve to a neighboring achievable rate (80 or 40 MHz region). The panel's rating of "works at 60 MHz" in practice means it tolerates overclocked writes; validate on hardware and drop `freq_write` to `40000000` if you ever see pixel corruption. ST7789's datasheet write-cycle spec is 66 ns (~15 MHz) — everything above that is overclocking that this panel family is widely known to tolerate.
- **Frame buffer budget (no PSRAM).** A full 16-bpp frame is 240x240x2 = 115,200 bytes — allocatable in internal DRAM but it consumes most of the free heap once WiFi is up. For the template, prefer either (a) a full-frame **8-bpp palette sprite** (57,600 bytes) pushed with `pushSprite`, or (b) a half/strip 16-bpp sprite composited in bands. Use `sprite.setPsram(false)` (or simply have no PSRAM) and, for deterministic memory, `sprite.setBuffer(staticBuf, w, h, bpp)` with a statically allocated array.
- **Tearing/throughput.** `pushSprite` uses the DMA queue; at 40 MHz a full 240x240x16 frame streams in ~23 ms (~40 fps ceiling), during which the CPU is free thanks to DMA.
- **Backlight under arduino-esp32 3.x.** Do not drive GPIO25 with raw `ledcAttachPin` (the 2.x API is gone in core 3.x); `Light_PWM` with `invert = true` is the supported path and was explicitly fixed for IDF5/core-3.x in LovyanGFX v1.1.16.

## Sources (primary)

- LovyanGFX repo & activity: <https://github.com/lovyan03/LovyanGFX> — GitHub API: pushed 2026-08-07, 1,724 stars, 15 open issues; releases 1.2.26 (2026-07-22), 1.2.25 (2026-07-09), 1.2.24 (2026-06-24).
- LovyanGFX release notes (IDF5/core-3.x fixes): <https://github.com/lovyan03/LovyanGFX/releases> — v1.1.16 "fix Light_PWM backlight not works under arduino-esp32 v3.0 (IDF 5)"; IDF 5.3 fix; v1.2.20 IDF/HAL v6-compat work.
- LovyanGFX classic-ESP32 DMA implementation: <https://github.com/lovyan03/LovyanGFX/blob/master/src/lgfx/v1/platforms/esp32/Bus_SPI.cpp> — classic-ESP32 DMA channel via `DPORT_SPI_DMA_CHAN_SEL_REG`, `spicommon_dmaworkaround_idle`, `SPI_DMA_CH_AUTO` (IDF >= 4.3), IDF5 code paths, DMA queue.
- LovyanGFX sprite static buffers: <https://github.com/lovyan03/LovyanGFX/blob/master/src/lgfx/v1/LGFX_Sprite.hpp> — `void setBuffer(void* buffer, int32_t w, int32_t h, uint8_t bpp = 0)`, `setPsram(bool)`.
- Panel_ST7789 defaults: <https://github.com/lovyan03/LovyanGFX/blob/master/src/lgfx/v1/panel/Panel_ST7789.hpp> — `_cfg.panel_height = _cfg.memory_height = 320;` (base for automatic rotation offsets when panel_height is overridden to 240).
- TFT_eSPI repo & releases: <https://github.com/Bodmer/TFT_eSPI>, <https://github.com/Bodmer/TFT_eSPI/releases> — GitHub API: last release V2.5.43 published 2024-03-06; 4,868 stars; 351 open issues. README: core "2.x.x or later" requirement, "DMA only on ESP32 S3 at the moment", S3 DMA fix "tested with the Arduino 3.3.6 board package".
- TFT_eSPI core-3.x breakage: <https://github.com/Bodmer/TFT_eSPI/issues/3329> (broken on cores > 2.0.14), <https://github.com/Bodmer/TFT_eSPI/issues/3355> (fails to compile on arduino-esp32 3.0.0).
- TFT_eSPI DMA-under-IDF background: <https://github.com/Bodmer/TFT_eSPI/issues/1301>, <https://github.com/Bodmer/TFT_eSPI/discussions/1304> (ESP32 DMA not enabled under IDF builds without `-DESP32=1`).
- Arduino_GFX repo & activity: <https://github.com/moononournation/Arduino_GFX> — GitHub API: pushed 2026-08-06, 1,123 stars; releases v1.6.7 (2026-07-18), v1.6.6 (2026-06-11). README: ST7789 240x240 supported; `ESP32LCD8/16`/`ESP32RGBPanel` parallel buses arduino-esp32 2.x-only.
- Arduino_GFX classic-ESP32 DMA bus: <https://github.com/moononournation/Arduino_GFX/blob/master/src/databus/Arduino_ESP32SPIDMA.h> — compiles for `CONFIG_IDF_TARGET_ESP32`, `SPI_DMA_CH_AUTO`, 1024-pixel max per transaction.
- bb_spi_lcd repo: <https://github.com/bitbank2/bb_spi_lcd> — GitHub API: pushed 2026-05-23, 233 stars.
