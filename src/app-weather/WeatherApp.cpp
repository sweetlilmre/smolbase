// The Weather Clock App (wayfinder map #63) — a port of the SmolTV-Pro
// weather dashboard onto the smolbase extension surface. Built by
// [env:weatherclock]; the stock demo in src/app/ stays untouched.
//
#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"
#include "../core/Secrets.h"
#include "WeatherData.h"
#include "WeatherKeys.h"
#include "WeatherScreen.h"
#include <Arduino.h>
#include <PsychicHttp.h>

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
  // TLS heap churn competes with the OTA path. WeatherScreen keeps its OWN
  // parked flag: Display ticks the screen independently of App::loop(), so
  // one flag can't guard both paths.
  bool parked = false;

public:
  void setup() override {
    ConfigStore::setAppNote(
        "Weather clock settings — the OpenWeatherMap API key lives in the Secrets tab.");
    // The #68 schema; keys and machine defaults come from WeatherKeys.h (#98).
    // col_hour/col_min deliberately reuse the stock app's keys so colours
    // carry across apps on one device.
    ConfigStore::registerColor(SettingSection::App, WxKeys::COL_HOUR, "Clock hour color",
                               WxKeys::DEF_COL_HOUR);
    ConfigStore::registerColor(SettingSection::App, WxKeys::COL_MIN, "Clock minute color",
                               WxKeys::DEF_COL_MIN);
    ConfigStore::registerColor(SettingSection::App, WxKeys::COL_SEC, "Seconds color",
                               WxKeys::DEF_COL_SEC);
    ConfigStore::registerBool(SettingSection::App, WxKeys::H24, "24-hour clock", WxKeys::DEF_H24);
    ConfigStore::registerChoice(SettingSection::App, WxKeys::DATE_FMT, "Date format", "DD/MM/YYYY",
                                WxKeys::DEF_DATE_FMT, DATE_FMTS, 5);
    ConfigStore::registerString(SettingSection::App, WxKeys::CITY, "City (name or OWM id)",
                                WxKeys::DEF_CITY);
    ConfigStore::registerString(SettingSection::App, WxKeys::NICKNAME, "Display name override", "");
    ConfigStore::registerChoice(SettingSection::App, WxKeys::UNIT_TEMP, "Temperature unit",
                                "\xC2\xB0"
                                "C",
                                WxKeys::DEF_UNIT_TEMP, TEMP_UNITS, 2);
    ConfigStore::registerChoice(SettingSection::App, WxKeys::UNIT_WIND, "Wind unit", "m/s",
                                WxKeys::DEF_UNIT_WIND, WIND_UNITS, 3);
    ConfigStore::registerChoice(SettingSection::App, WxKeys::UNIT_PRESS, "Pressure unit", "hPa",
                                WxKeys::DEF_UNIT_PRESS, PRESS_UNITS, 4);
    ConfigStore::registerInt(SettingSection::App, WxKeys::INTERVAL, "Weather refresh (min)",
                             WxKeys::DEF_INTERVAL_MIN, 5, 120);
    // The one Secret Descriptor this app consumes (ADR 0003): declaring it
    // here is all it takes for the Settings UI to grow a Secrets section.
    Secrets::describe(WxKeys::OWM_KEY, "OpenWeatherMap API key",
                      "Optional — without a key, weather falls back to Open-Meteo "
                      "(no humidity, pressure, or feels-like).");
    // RAM choreography (#74/#94): WeatherData owns the fetch window and
    // announces it — free the marquee sprite ahead of the ~49 KB TLS peak,
    // restore it once the cycle's result lands.
    WeatherData::begin([this] { screen.suspendMarquee(); },
                       [this] { screen.resumeMarquee(); });
    screen.begin();
    Display::setActive(&screen);
  }

  void registerRoutes(PsychicHttpServer& server) override {
    server.on("/api/debug/weather", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
      JsonDocument doc;
      WeatherData::debugJson(doc);
      String out;
      serializeJson(doc, out);
      return res->send(200, "application/json", out.c_str());
    });
  }

  void loop() override {
    if (parked) return;
    WeatherData::loop(); // fetch-window hooks fire from inside (see setup)
    if (const WeatherData::Reading* r = WeatherData::takeChanged()) screen.showReading(*r);
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
