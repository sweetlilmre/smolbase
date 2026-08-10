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

// The panel is write-only — a "screenshot" is a re-render into a sprite,
// encoded as an indexed BMP into LittleFS, served back. Preferred depth is
// 8 bpp (57.6 KB); when the heap has no block that size (TLS + marquee
// fragmentation, seen on-device), fall back to a 4-bpp 16-color render —
// coarse colors, exact geometry, which is what layout debugging needs.
// Debug-only: runs on the httpd task; races with the live tick are benign.

// The 4-bpp palette: the dashboard's known colors plus a few icon-ish tones;
// palette targets quantize every draw color to the nearest entry.
constexpr uint32_t SHOT_PAL16[16] = {0x000000, 0xffffff, 0xfeba00, 0x99ffff, 0x99ff1f, 0x87cefa,
                                     0x2196f3, 0xe53935, 0xff5a00, 0xff5900, 0x808080, 0x404040,
                                     0xffd700, 0xffa500, 0x1c355c, 0x9fc7ff};

esp_err_t sendScreenshot(PsychicResponse* res) {
  lgfx::LGFX_Sprite shot;
  int bpp = 8;
  shot.setColorDepth(8);
  if (!shot.createSprite(240, 240)) {
    bpp = 4;
    shot.setColorDepth(4);
    if (!shot.createSprite(240, 240))
      return res->send(503, "text/plain", "not enough heap for a screenshot");
    shot.createPalette();
    for (int i = 0; i < 16; ++i)
      shot.setPaletteColor(i, SHOT_PAL16[i] >> 16, (SHOT_PAL16[i] >> 8) & 0xff,
                           SHOT_PAL16[i] & 0xff);
  }
  // Freeze the live tick: both renders walk the same stateful font wrappers.
  s_screen->pause(true);
  vTaskDelay(pdMS_TO_TICKS(50)); // let an in-flight tick finish
  s_screen->renderTo(shot);
  s_screen->pause(false);

  File f = LittleFS.open("/shot.bmp", "w");
  if (!f) {
    shot.deleteSprite();
    return res->send(500, "text/plain", "LittleFS open failed");
  }
  const int entries = 1 << bpp;
  const uint32_t rowBytes = 240 * bpp / 8; // 240 and 120: both % 4 == 0
  const uint32_t dataSize = rowBytes * 240;
  const uint32_t offset = 14 + 40 + entries * 4;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  uint32_t v = offset + dataSize;
  memcpy(hdr + 2, &v, 4);            // file size
  memcpy(hdr + 10, &offset, 4);      // pixel data offset
  v = 40; memcpy(hdr + 14, &v, 4);   // BITMAPINFOHEADER size
  int32_t d = 240; memcpy(hdr + 18, &d, 4); memcpy(hdr + 22, &d, 4); // w, h
  hdr[26] = 1; hdr[28] = (uint8_t)bpp; // planes, bpp
  memcpy(hdr + 34, &dataSize, 4);
  v = entries; memcpy(hdr + 46, &v, 4); // palette entries
  f.write(hdr, sizeof(hdr));
  for (int i = 0; i < entries; ++i) { // palette, BGRA order
    uint8_t pal[4];
    if (bpp == 8) { // RGB332 identity
      pal[0] = (uint8_t)((i & 0x03) * 255 / 3);
      pal[1] = (uint8_t)(((i >> 2) & 0x07) * 255 / 7);
      pal[2] = (uint8_t)(((i >> 5) & 0x07) * 255 / 7);
    } else {
      pal[0] = SHOT_PAL16[i] & 0xff;
      pal[1] = (SHOT_PAL16[i] >> 8) & 0xff;
      pal[2] = SHOT_PAL16[i] >> 16;
    }
    pal[3] = 0;
    f.write(pal, 4);
  }
  const uint8_t* fb = (const uint8_t*)shot.getBuffer();
  for (int y = 239; y >= 0; --y) f.write(fb + y * rowBytes, rowBytes); // bottom-up
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
    // Font metric probe: the WX_CLOCK96 bin carries adv=37 for '0' (verified
    // host-side); if the loader reports way less here, the BFF advance path
    // is the corruption. Uses a throwaway 8x8 sprite as the metric context.
    server.on("/api/debug/fonts", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
      JsonDocument doc;
      WeatherScreen::fontProbe(doc);
      String out;
      serializeJson(doc, out);
      return res->send(200, "application/json", out.c_str());
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
