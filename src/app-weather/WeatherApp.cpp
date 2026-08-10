// The Weather Clock App (wayfinder map #63) — a port of the SmolTV-Pro
// weather dashboard onto the smolbase extension surface. Built by
// [env:weatherclock]; the stock demo in src/app/ stays untouched.
//
#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"
#include "../core/Secrets.h"
#include "WeatherData.h"
#include "WeatherScreen.h"
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

class WeatherApp : public App {
  WeatherScreen screen;
  // OtaStarting parks the app (docs/building-your-app.md): drawing stops —
  // mandatory here, our art lives in .rodata and flash reads during an OTA
  // write can panic (#53) — and loop() stops scheduling fetches, so no new
  // TLS heap churn competes with the OTA path.
  bool parked = false;

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
    screen.begin();
    Display::setActive(&screen);
  }

  void loop() override {
    if (parked) return;
    WeatherData::loop();
    if (WeatherData::changed()) screen.markWeatherDirty();
  }

  void onSystemEvent(SysEvent e) override {
    if (e == SysEvent::OtaStarting) {
      parked = true;
      screen.park();
      return;
    }
    if (parked) return; // no un-park: OTA ends in a reboot
    if (e == SysEvent::NetworkUp) WeatherData::forceRefresh();
    if (e == SysEvent::SettingsChanged) {
      WeatherData::onSettingsChanged();
      screen.loadSettings(); // units/colours/formats live-apply from cache
    }
  }
};

} // namespace

App& makeApp() {
  static WeatherApp app;
  return app;
}
