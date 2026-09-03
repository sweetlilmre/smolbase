// The weather dashboard Screen (#71): the #67 prototype's variant C —
// centered symmetric composition — bound to the WeatherData layer.
//
// Rendering (ADR 0004, #103): the screen owns a static 240×64 RGB565 scratch
// sprite — one clock band tall, the tallest indivisible band. Each band
// painter composes at band-relative y in the scratch and pushes it to the
// panel at the band's real y, clipped to the band height: clears never touch
// the live panel (no flicker) and a push is a few ms of SPI. The clock band
// (hh:mm + seconds, or the identity overlay) redraws whole every second; the
// marquee strip repaints at 30 Hz with a time-based scroll. The core
// framebuffer is compiled out of this env (SMOLBASE_FRAMEBUFFER=0) and there
// is no heap sprite — the fetch-window RAM choreography (#74/#94) stays
// retired.
#pragma once
#include "../core/App.h"
#include "WeatherData.h"
#include <LovyanGFX.hpp>
#include <string>

// Unit prefs (#68 catalog values) as one bundle. Filled by loadSettings()
// — which begin() calls before any render — so no member defaults here:
// the defaults live at the getString calls, in one place.
struct WxUnits {
  std::string temp, wind, press;
};

class WeatherScreen : public Screen {
public:
  using Units = WxUnits;

  void begin(); // load fonts, bind the scratch buffer; call once from setup
  void loadSettings();
  // The one entry point for new data (#97): cache the reading and schedule
  // the weather bands' recompose — the two were only ever meaningful together.
  void showReading(const WeatherData::Reading& r) {
    cachedReading = r;
    weatherDirty = true;
  }
  // OtaStarting: stop touching flash/panel. WeatherApp holds a sibling parked
  // flag for its loop() — Display ticks this screen independently, so the two
  // flags are deliberate duplication, set together by onSystemEvent.
  void park() { parked = true; }
  void onEnter(lgfx::LGFX_Device& gfx) override;
  void tick(lgfx::LGFX_Device& gfx) override;
  void onTap() override;       // force weather refresh (charter)
  void onLongPress() override; // show identity overlay for OVERLAY_MS

private:
  // Band painters (ADR 0004): each composes its band into the scratch and
  // pushes only that band's rows to the panel.
  void drawWeather(lgfx::LovyanGFX& gfx); // top band (two passes) + gauge band
  void drawMarquee(lgfx::LovyanGFX& gfx, int scrollPx);
  void drawClockBand(lgfx::LovyanGFX& gfx, const struct tm& tm); // clock/seconds or overlay
  void drawDate(lgfx::LovyanGFX& gfx, const struct tm& tm);

  WeatherData::Reading cachedReading;
  // Set by loadSettings() — which begin() calls before any render — so no
  // member defaults: those live at the WeatherKeys.h-cited reads, once (#98).
  uint32_t colHour = 0, colMin = 0, colSec = 0;
  bool h24 = true;
  std::string dateFmt;
  std::string nickname;
  Units units;
  // Change-detection caches: -1 differs from every real value, so it doubles
  // as "force repaint". The clock band redraws whole on every seconds change
  // (ADR 0004), so lastSec alone drives it — there is no lastMin.
  int lastSec = -1, lastDay = -1;
  bool weatherDirty = true;
  uint32_t lastScrollMs = 0; // marquee scroll clock: 1 px per 33 ms, time-based
  int marqX = 0;             // marquee scroll offset
  bool parked = false;
  uint32_t overlayUntilMs = 0; // non-zero while the identity overlay is visible
  bool overlayDirty = false;   // draw requested by onLongPress, consumed by tick()
};
