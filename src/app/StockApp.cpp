// YOUR APP GOES HERE. This file is the worked example consumers gut and replace:
// a Screen showing the device's identity (IP, hostname, time), wired up through
// every extension hook the template offers.
#include "../core/App.h"
#include "../core/Clock.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"
#include "../core/Net.h"
#include <Arduino.h>
#include <ctime>

namespace {

// "#RRGGBB" -> 24-bit RGB; anything malformed falls back. The settings store
// keeps colors as the same string an <input type="color"> speaks, so the web
// side needs no conversion — parsing happens once per repaint, here.
uint32_t hexRgb(const String& s, uint32_t fallback) {
  if (s.length() == 7 && s[0] == '#') {
    char* end;
    long v = strtol(s.c_str() + 1, &end, 16);
    if (*end == '\0') return (uint32_t)v;
  }
  return fallback;
}

class StockScreen : public Screen {
  bool dirty = true;
  int lastMinute = -1;
  bool colonOn = true;
  // Colors come from app-registered settings (see StockApp::setup); cached as
  // 24-bit RGB and re-read on every full repaint — SettingsChanged marks
  // dirty, so a picker change on the web page lands on the panel live.
  uint32_t colHour = 0xffffff, colMin = 0xffffff, colColon = 0xffffff,
           colHost = 0xffffff, colIp = 0xffffff;

  void loadColors() {
    colHour = hexRgb(ConfigStore::getString("col_hour"), 0xffffff);
    colMin = hexRgb(ConfigStore::getString("col_min"), 0xffffff);
    colColon = hexRgb(ConfigStore::getString("col_colon"), 0xffffff);
    colHost = hexRgb(ConfigStore::getString("col_host"), 0xffffff);
    colIp = hexRgb(ConfigStore::getString("col_ip"), 0xffffff);
  }

  // The colon blinks at 1 Hz as visible proof the screen is live. Only its
  // own cell is overdrawn — the digits are never touched between minutes.
  // "HH:MM" is drawn centered, HH and MM are equal-width, so the colon sits
  // exactly at x=120; textWidth(":") bounds the cell without clipping digits.
  void drawColon(lgfx::LGFX_Device& d) {
    d.setFont(&fonts::FreeSansBold24pt7b);
    int w = d.textWidth(":");
    d.fillRect(120 - w / 2, 70, w, 60, TFT_BLACK);
    if (colonOn) {
      d.setTextColor(d.color888(colColon >> 16, (colColon >> 8) & 0xff, colColon & 0xff), TFT_BLACK);
      d.setTextDatum(lgfx::middle_center);
      d.drawString(":", 120, 100);
    }
  }

public:
  void markDirty() { dirty = true; }

  void onEnter(lgfx::LGFX_Device& d) override {
    d.fillScreen(TFT_BLACK);
    dirty = true;
    lastMinute = -1;
  }

  void tick(lgfx::LGFX_Device& d) override {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    // Solid colon until first sync: blinking implies the clock is ticking.
    bool colonNow = !Clock::isSynced() || (t.tm_sec & 1) == 0;
    if (!dirty && t.tm_min == lastMinute) {
      if (colonNow != colonOn) { // 1 Hz heartbeat, colon cell only
        colonOn = colonNow;
        drawColon(d);
      }
      return;
    }
    lastMinute = t.tm_min;
    dirty = false;
    colonOn = colonNow;
    loadColors();

    // Hour and minute wear their own colors, so they are drawn as separate
    // strings hung off the colon cell instead of one centered "HH:MM".
    d.setFont(&fonts::FreeSansBold24pt7b);
    int w = d.textWidth(":");
    char hh[3] = "--", mm[3] = "--";
    if (Clock::isSynced()) {
      snprintf(hh, sizeof(hh), "%02d", t.tm_hour);
      snprintf(mm, sizeof(mm), "%02d", t.tm_min);
    }
    d.fillRect(0, 70, 240, 60, TFT_BLACK);
    d.setTextColor(d.color888(colHour >> 16, (colHour >> 8) & 0xff, colHour & 0xff), TFT_BLACK);
    d.setTextDatum(lgfx::middle_right);
    d.drawString(hh, 120 - w / 2, 100);
    d.setTextColor(d.color888(colMin >> 16, (colMin >> 8) & 0xff, colMin & 0xff), TFT_BLACK);
    d.setTextDatum(lgfx::middle_left);
    d.drawString(mm, 120 + w / 2, 100);
    drawColon(d); // honor the current blink phase on full repaints too

    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextDatum(lgfx::middle_center);
    d.fillRect(0, 145, 240, 65, TFT_BLACK); // covers 9pt ascenders/descenders fully
    d.setTextColor(d.color888(colHost >> 16, (colHost >> 8) & 0xff, colHost & 0xff), TFT_BLACK);
    d.drawString(Net::deviceName() + ".local", 120, 160);
    d.setTextColor(d.color888(colIp >> 16, (colIp >> 8) & 0xff, colIp & 0xff), TFT_BLACK);
    d.drawString(Net::isUp() ? Net::ip().toString() : "connecting...", 120, 185);
  }

  void onTap() override { markDirty(); } // demo: any tap forces a repaint
};

class StockApp : public App {
  StockScreen screen;

public:
  void setup() override {
    // The app's own settings: registered into the shared schema, persisted in
    // settings.json, editable from the served pages (color pickers on
    // index.html, plain fields in the settings UI's App section) — the
    // app/config/html triangle in four lines.
    ConfigStore::registerString(SettingSection::App, "col_hour", "Clock hour color", "#ffffff");
    ConfigStore::registerString(SettingSection::App, "col_min", "Clock minute color", "#ffffff");
    ConfigStore::registerString(SettingSection::App, "col_colon", "Clock colon color", "#ffffff");
    ConfigStore::registerString(SettingSection::App, "col_host", "Hostname color", "#ffffff");
    ConfigStore::registerString(SettingSection::App, "col_ip", "IP address color", "#ffffff");
    Display::setActive(&screen);
  }

  void onSystemEvent(SysEvent e) override {
    // SettingsChanged matters too: the hostname shown on screen can change at
    // runtime. The idempotent-repaint pattern makes "which setting?" moot.
    if (e == SysEvent::NetworkUp || e == SysEvent::TimeSynced ||
        e == SysEvent::SettingsChanged) {
      screen.markDirty();
    }
  }
};

} // namespace

App& makeApp() {
  static StockApp app;
  return app;
}
