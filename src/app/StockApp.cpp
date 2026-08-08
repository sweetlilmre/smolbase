// YOUR APP GOES HERE. This file is the worked example consumers gut and replace:
// a Screen showing the device's identity (IP, hostname, time), wired up through
// every extension hook the template offers.
#include "../core/App.h"
#include "../core/Clock.h"
#include "../core/Display.h"
#include "../core/Net.h"
#include <Arduino.h>
#include <ctime>

namespace {

class StockScreen : public Screen {
  bool dirty = true;
  int lastMinute = -1;
  bool colonOn = true;

  // The colon blinks at 1 Hz as visible proof the screen is live. Only its
  // own cell is overdrawn — the digits are never touched between minutes.
  // "HH:MM" is drawn centered, HH and MM are equal-width, so the colon sits
  // exactly at x=120; textWidth(":") bounds the cell without clipping digits.
  void drawColon(lgfx::LGFX_Device& d) {
    d.setFont(&fonts::FreeSansBold24pt7b);
    int w = d.textWidth(":");
    d.fillRect(120 - w / 2, 70, w, 60, TFT_BLACK);
    if (colonOn) {
      d.setTextColor(TFT_WHITE, TFT_BLACK);
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

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(lgfx::middle_center);

    d.setFont(&fonts::FreeSansBold24pt7b);
    char clock[6] = "--:--";
    if (Clock::isSynced()) snprintf(clock, sizeof(clock), "%02d:%02d", t.tm_hour, t.tm_min);
    d.fillRect(0, 70, 240, 60, TFT_BLACK);
    d.drawString(clock, 120, 100);
    drawColon(d); // honor the current blink phase on full repaints too

    d.setFont(&fonts::FreeSans9pt7b);
    d.fillRect(0, 145, 240, 65, TFT_BLACK); // covers 9pt ascenders/descenders fully
    d.drawString(Net::deviceName() + ".local", 120, 160);
    d.drawString(Net::isUp() ? Net::ip().toString() : "connecting...", 120, 185);
  }

  void onTap() override { markDirty(); } // demo: any tap forces a repaint
};

class StockApp : public App {
  StockScreen screen;

public:
  void setup() override { Display::setActive(&screen); }

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
