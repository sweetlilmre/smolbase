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
    if (!dirty && t.tm_min == lastMinute) return; // draw only on change
    lastMinute = t.tm_min;
    dirty = false;

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(lgfx::middle_center);

    d.setFont(&fonts::FreeSansBold24pt7b);
    char clock[6] = "--:--";
    if (Clock::isSynced()) snprintf(clock, sizeof(clock), "%02d:%02d", t.tm_hour, t.tm_min);
    d.fillRect(0, 70, 240, 60, TFT_BLACK);
    d.drawString(clock, 120, 100);

    d.setFont(&fonts::FreeSans9pt7b);
    d.fillRect(0, 150, 240, 60, TFT_BLACK);
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
    if (e == SysEvent::NetworkUp || e == SysEvent::TimeSynced) screen.markDirty();
  }
};

} // namespace

App& makeApp() {
  static StockApp app;
  return app;
}
