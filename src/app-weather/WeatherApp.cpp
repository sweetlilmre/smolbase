// The Weather Clock App (wayfinder map #63) — a port of the SmolTV-Pro
// weather dashboard onto the smolbase extension surface. Built by
// [env:weatherclock]; the stock demo in src/app/ stays untouched.
//
// STUB (#69): proves the two-app build wiring — compiles, links makeApp(),
// paints a placeholder. The dashboard Screen (#71), data layer (#70), and
// settings schema (#68) land on top of this.
#include "../core/App.h"
#include "../core/Display.h"
#include "../core/Secrets.h"
#include <Arduino.h>

namespace {

class PlaceholderScreen : public Screen {
public:
  void onEnter(lgfx::LGFX_Device& gfx) override {
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextDatum(lgfx::middle_center);
    gfx.setTextSize(2);
    gfx.drawString("weather clock", 120, 110);
    gfx.setTextSize(1);
    gfx.drawString("under construction (#63)", 120, 140);
  }
};

class WeatherApp : public App {
  PlaceholderScreen screen;

public:
  void setup() override {
    // The one Secret Descriptor this app consumes (ADR 0003): declaring it
    // here is all it takes for the Settings UI to grow a Secrets section.
    // The data layer (#70) reads it back with Secrets::get("owm_api_key").
    Secrets::describe("owm_api_key", "OpenWeatherMap API key",
                      "Optional — without a key, weather falls back to Open-Meteo "
                      "(no humidity, pressure, or feels-like).");
    Display::setActive(&screen);
  }
};

} // namespace

App& makeApp() {
  static WeatherApp app;
  return app;
}
