// The weather dashboard Screen (#71): the #67 prototype's variant C —
// centered symmetric composition — bound to the WeatherData layer.
#pragma once
#include "../core/App.h"
#include "WeatherData.h"
#include <LovyanGFX.hpp>

// Unit prefs (#68 catalog values) as one bundle. Filled by loadSettings()
// — which begin() calls before any render — so no member defaults here:
// the defaults live at the getString calls, in one place.
struct WxUnits {
  String temp, wind, press;
};

// The scrolling info line (#95): an 8-bpp sprite holding one copy of the
// text, tiled across its band at a scrolling offset. Sole owner of the
// sprite + width + offset trio, enforcing the invariant the previous loose
// members couldn't: width is non-zero iff the buffer is live AND holds a
// current line. rebuild() while suspended is therefore a safe no-op — the
// path that used to draw into a deleted sprite when a settings save landed
// mid-fetch. Methods live in WeatherScreen.cpp (they use its band layout,
// palette, and once-only-allocation rationale).
class Marquee {
public:
  void create();  // allocate the sprite ONCE at begin(), pristine heap
  void suspend(); // free the sprite for the fetch's TLS peak (#74)
  void resume(const WeatherData::Reading& r, const WxUnits& u, const lgfx::IFont* f);
  void rebuild(const WeatherData::Reading& r, const WxUnits& u, const lgfx::IFont* f);
  void render(lgfx::LovyanGFX& gfx, uint32_t now); // ~30 Hz; sole writer of the offset

private:
  lgfx::LGFX_Sprite spr;
  int width = 0; // non-zero iff spr has a buffer holding a current line
  int x = 0;
  uint32_t lastFrameMs = 0;
};

class WeatherScreen : public Screen {
public:
  using Units = WxUnits;

  void begin();   // load fonts; call once from setup
  void loadSettings();
  void markWeatherDirty() { weatherDirty = true; }
  void setReading(const WeatherData::Reading& r) { cachedReading = r; }
  // OtaStarting: stop touching flash/panel. WeatherApp holds a sibling parked
  // flag for its loop() — Display ticks this screen independently, so the two
  // flags are deliberate duplication, set together by onSystemEvent.
  void park() { parked = true; }
  // RAM choreography (#74): the marquee sprite is the app's one big heap
  // block (13.4 KB), and the TLS handshake needs every byte — the app frees
  // it for the duration of a fetch and restores it after. The marquee
  // freezes off-screen for those few seconds each refresh interval.
  void suspendMarquee();
  void resumeMarquee();
  void onEnter(lgfx::LGFX_Device& gfx) override;
  void tick(lgfx::LGFX_Device& gfx) override;
  void onTap() override;      // force weather refresh (charter)
  void onLongPress() override; // show identity overlay for OVERLAY_MS

private:
  // Painters are functions of (gfx, time value) — tick() is the only reader
  // of the wall clock; the last* members below are change-detection caches.
  void drawWeather(lgfx::LovyanGFX& gfx); // icon, city, badge, gauges, marquee text
  void drawClock(lgfx::LovyanGFX& gfx, const struct tm& tm);
  void drawSeconds(lgfx::LovyanGFX& gfx, int sec);
  void drawDate(lgfx::LovyanGFX& gfx, const struct tm& tm);
  void drawIdentityOverlay(lgfx::LovyanGFX& gfx);

  WeatherData::Reading cachedReading;
  uint32_t colHour = 0xffffff, colMin = 0xff5a00, colSec = 0xff5900;
  bool h24 = true;
  String dateFmt = "%d/%m/%Y";
  String nickname;
  Units units;

  Marquee marquee;
  int lastMin = -1, lastSec = -1, lastDay = -1;
  bool weatherDirty = true;
  bool parked = false;
  uint32_t overlayUntilMs = 0; // non-zero while the identity overlay is visible
  bool overlayDirty = false;   // draw requested by onLongPress, consumed by tick()
};
