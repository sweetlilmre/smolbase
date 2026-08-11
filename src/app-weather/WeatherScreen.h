// The weather dashboard Screen (#71): the #67 prototype's variant C —
// centered symmetric composition — bound to the WeatherData layer.
#pragma once
#include "../core/App.h"
#include <LovyanGFX.hpp>

class WeatherScreen : public Screen {
public:
  void begin();   // load fonts; call once from setup
  void loadSettings();
  void markWeatherDirty() { weatherDirty = true; }
  void park() { parked = true; } // OtaStarting: stop touching flash/panel
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
  void drawWeather(lgfx::LovyanGFX& gfx); // icon, city, badge, gauges, marquee text
  void drawClock(lgfx::LovyanGFX& gfx);
  void drawSeconds(lgfx::LovyanGFX& gfx);
  void drawDate(lgfx::LovyanGFX& gfx);
  void drawIdentityOverlay(lgfx::LovyanGFX& gfx);
  void rebuildMarquee();

  uint32_t colHour = 0xffffff, colMin = 0xff5a00, colSec = 0xff5900;
  bool h24 = true;
  String dateFmt = "%d/%m/%Y";
  String nickname;

  lgfx::LGFX_Sprite marq;  // 8-bpp line sprite, fixed size, allocated once
  int marqWidth = 0;       // pixel width of one marquee copy
  int marqX = 0;
  uint32_t lastFrameMs = 0;
  int lastMin = -1, lastSec = -1, lastDay = -1;
  bool weatherDirty = true;
  bool parked = false;
  uint32_t overlayUntilMs = 0; // non-zero while the identity overlay is visible
  bool overlayDirty = false;   // draw requested by onLongPress, consumed by tick()
};
