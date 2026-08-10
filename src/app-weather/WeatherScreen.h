// The weather dashboard Screen (#71): the #67 prototype's variant C —
// centered symmetric composition — bound to the WeatherData layer.
#pragma once
#include "../core/App.h"
#include <LovyanGFX.hpp>

class WeatherScreen : public Screen {
public:
  void begin();   // load fonts, build the marquee sprite; call once from setup
  void loadSettings();
  void markWeatherDirty() { weatherDirty = true; }

  void onEnter(lgfx::LGFX_Device& gfx) override;
  void tick(lgfx::LGFX_Device& gfx) override;
  void onTap() override; // force weather refresh (charter)

private:
  void drawWeather(lgfx::LGFX_Device& gfx); // icon, city, badge, gauges, marquee text
  void drawClock(lgfx::LGFX_Device& gfx);
  void drawSeconds(lgfx::LGFX_Device& gfx);
  void drawDate(lgfx::LGFX_Device& gfx);
  void rebuildMarquee();

  uint32_t colHour = 0xffffff, colMin = 0xff5a00, colSec = 0xff5900;
  bool h24 = true;
  String dateFmt = "%d/%m/%Y";
  String nickname;

  lgfx::LGFX_Sprite marq;  // 16-bpp strip, scrolled each frame
  int marqWidth = 0;       // pixel width of one marquee copy
  int marqX = 0;
  uint32_t lastFrameMs = 0;
  int lastMin = -1, lastSec = -1, lastDay = -1;
  bool weatherDirty = true;
};
