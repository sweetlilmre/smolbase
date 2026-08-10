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
#include <LittleFS.h>
#include <PsychicHttp.h>

namespace {

// Route handlers are captureless (PsychicHttp callback style in this repo),
// so the screen they render reaches them through this pointer.
WeatherScreen* s_screen = nullptr;

// The panel is write-only — a "screenshot" is a re-render into an 8-bpp
// (RGB332) sprite, encoded as a 256-color BMP into LittleFS, served back.
// Debug-only: runs on the httpd task; races with the live tick are benign.
esp_err_t sendScreenshot(PsychicResponse* res) {
  lgfx::LGFX_Sprite shot;
  shot.setColorDepth(8);
  if (!shot.createSprite(240, 240)) // 57.6 KB — the biggest transient we take
    return res->send(503, "text/plain", "not enough heap for a screenshot");
  s_screen->renderTo(shot);

  File f = LittleFS.open("/shot.bmp", "w");
  if (!f) {
    shot.deleteSprite();
    return res->send(500, "text/plain", "LittleFS open failed");
  }
  const uint32_t dataSize = 240 * 240; // 8 bpp, 240 % 4 == 0: no row padding
  const uint32_t offset = 14 + 40 + 256 * 4;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  uint32_t v = offset + dataSize;
  memcpy(hdr + 2, &v, 4);            // file size
  memcpy(hdr + 10, &offset, 4);      // pixel data offset
  v = 40; memcpy(hdr + 14, &v, 4);   // BITMAPINFOHEADER size
  int32_t d = 240; memcpy(hdr + 18, &d, 4); memcpy(hdr + 22, &d, 4); // w, h
  hdr[26] = 1; hdr[28] = 8;          // planes, bpp
  memcpy(hdr + 34, &dataSize, 4);
  v = 256; memcpy(hdr + 46, &v, 4);  // palette entries
  f.write(hdr, sizeof(hdr));
  for (int i = 0; i < 256; ++i) {    // RGB332 identity palette, BGRA order
    uint8_t pal[4] = {(uint8_t)((i & 0x03) * 255 / 3), (uint8_t)(((i >> 2) & 0x07) * 255 / 7),
                      (uint8_t)(((i >> 5) & 0x07) * 255 / 7), 0};
    f.write(pal, 4);
  }
  const uint8_t* fb = (const uint8_t*)shot.getBuffer();
  for (int y = 239; y >= 0; --y) f.write(fb + y * 240, 240); // BMP rows bottom-up
  f.close();
  shot.deleteSprite();

  PsychicFileResponse file(res, LittleFS, "/shot.bmp");
  return file.send();
}

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
    s_screen = &screen;
    Display::setActive(&screen);
  }

  // Debug surface for the OTA-only device (#74): fetch state as JSON, and a
  // re-rendered screenshot — the write-only panel can't be read back.
  void registerRoutes(PsychicHttpServer& server) override {
    server.on("/api/debug/weather", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
      JsonDocument doc;
      WeatherData::debugJson(doc);
      String out;
      serializeJson(doc, out);
      return res->send(200, "application/json", out.c_str());
    });
    server.on("/api/debug/screenshot", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
      return sendScreenshot(res);
    });
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
