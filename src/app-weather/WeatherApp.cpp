// The Weather Clock App (wayfinder map #63) — a port of the SmolTV-Pro
// weather dashboard onto the smolbase extension surface. Built by
// [env:weatherclock]; the stock demo in src/app/ stays untouched.
//
// Current state: settings schema (#68) + data layer (#70) are real; the
// Screen is still the #69 placeholder, upgraded just enough to prove the
// data path on-device. The dashboard visuals land with #71 (after the #67
// prototype); until then this text readout is deliberately throwaway.
#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"
#include "../core/Secrets.h"
#include "WeatherData.h"
#include <Arduino.h>

namespace {

constexpr SettingChoice DATE_FMTS[] = {
    // Values are strftime strings (#68): the firmware formats straight from
    // the stored value; the label is only for humans.
    {"DD/MM/YYYY", "%d/%m/%Y"}, {"YYYY/MM/DD", "%Y/%m/%d"}, {"MM/DD/YYYY", "%m/%d/%Y"},
    {"MM/DD", "%m/%d"},         {"DD/MM", "%d/%m"},
};
constexpr SettingChoice TEMP_UNITS[] = {{"\xC2\xB0"
                                         "C",
                                         "C"},
                                        {"\xC2\xB0"
                                         "F",
                                         "F"}};
constexpr SettingChoice WIND_UNITS[] = {{"m/s", "ms"}, {"km/h", "kmh"}, {"mile/h", "mph"}};
constexpr SettingChoice PRESS_UNITS[] = {
    {"hPa", "hpa"}, {"kPa", "kpa"}, {"mmHg", "mmhg"}, {"inHg", "inhg"}};

// Placeholder Screen (#69, data-aware since #70): plain text readout of the
// latest Reading. Gutted wholesale by the real dashboard (#71).
class PlaceholderScreen : public Screen {
  bool dirty = true;

public:
  void markDirty() { dirty = true; }

  void onEnter(lgfx::LGFX_Device& gfx) override {
    gfx.fillScreen(TFT_BLACK);
    dirty = true;
  }

  void tick(lgfx::LGFX_Device& gfx) override {
    if (!dirty) return;
    dirty = false;
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextDatum(lgfx::middle_center);
    const WeatherData::Reading& r = WeatherData::reading();
    if (!r.valid) {
      gfx.setTextSize(2);
      gfx.drawString("weather clock", 120, 100);
      gfx.setTextSize(1);
      gfx.drawString("waiting for first fetch (#70)", 120, 130);
      return;
    }
    String name = ConfigStore::getString("nickname", "");
    if (!name.length()) name = r.city;
    gfx.setTextSize(2);
    gfx.drawString(name + (r.country[0] ? " " + String(r.country) : ""), 120, 60);
    gfx.drawString(WeatherData::fmtTemp(r.tempC) + "  " + r.condition, 120, 95);
    gfx.setTextSize(1);
    gfx.drawString("min " + WeatherData::fmtTemp(r.tempMinC) + "  max " +
                       WeatherData::fmtTemp(r.tempMaxC),
                   120, 125);
    gfx.drawString(WeatherData::fmtWind(r.windMs) + "  " + WeatherData::fmtPress(r.pressureHpa) +
                       "  " + String(r.humidity) + "%",
                   120, 145);
    gfx.drawString(r.keyless ? "source: Open-Meteo (keyless)" : "source: OpenWeatherMap", 120,
                   175);
  }

  // Tap = force refresh (charter); long-press stays a no-op.
  void onTap() override { WeatherData::forceRefresh(); }
};

class WeatherApp : public App {
  PlaceholderScreen screen;

public:
  void setup() override {
    ConfigStore::setAppNote(
        "Weather clock settings — the OpenWeatherMap API key lives in the Secrets tab.");
    // The #68 schema. col_hour/col_min deliberately reuse the stock app's
    // keys so colours carry across apps on one device.
    ConfigStore::registerColor(SettingSection::App, "col_hour", "Clock hour color", "#FFFFFF");
    ConfigStore::registerColor(SettingSection::App, "col_min", "Clock minute color", "#FF5A00");
    ConfigStore::registerColor(SettingSection::App, "col_sec", "Seconds color", "#FF5900");
    ConfigStore::registerBool(SettingSection::App, "h24", "24-hour clock", true);
    ConfigStore::registerChoice(SettingSection::App, "date_fmt", "Date format", "DD/MM/YYYY",
                                "%d/%m/%Y", DATE_FMTS, 5);
    ConfigStore::registerString(SettingSection::App, "city", "City (name or OWM id)", "Durban");
    ConfigStore::registerString(SettingSection::App, "nickname", "Display name override", "");
    ConfigStore::registerChoice(SettingSection::App, "unit_temp", "Temperature unit",
                                "\xC2\xB0"
                                "C",
                                "C", TEMP_UNITS, 2);
    ConfigStore::registerChoice(SettingSection::App, "unit_wind", "Wind unit", "m/s", "ms",
                                WIND_UNITS, 3);
    ConfigStore::registerChoice(SettingSection::App, "unit_press", "Pressure unit", "hPa", "hpa",
                                PRESS_UNITS, 4);
    ConfigStore::registerInt(SettingSection::App, "wx_interval", "Weather refresh (min)", 20, 5,
                             120);
    // The one Secret Descriptor this app consumes (ADR 0003): declaring it
    // here is all it takes for the Settings UI to grow a Secrets section.
    Secrets::describe("owm_api_key", "OpenWeatherMap API key",
                      "Optional — without a key, weather falls back to Open-Meteo "
                      "(no humidity, pressure, or feels-like).");
    WeatherData::begin();
    Display::setActive(&screen);
  }

  void loop() override {
    WeatherData::loop();
    if (WeatherData::changed()) screen.markDirty();
  }

  void onSystemEvent(SysEvent e) override {
    if (e == SysEvent::NetworkUp) WeatherData::forceRefresh();
    if (e == SysEvent::SettingsChanged) {
      WeatherData::onSettingsChanged();
      screen.markDirty(); // units/colours/formats live-apply from cache
    }
  }
};

} // namespace

App& makeApp() {
  static WeatherApp app;
  return app;
}
