// YOUR APP GOES HERE. This file is the worked example consumers gut and
// replace: thin glue wiring a screen (DemoScreen on framebuffer builds,
// StockScreen otherwise — each in its own file, gut them piecewise) to the
// system through every extension hook the template offers.
#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
#include "DemoScreen.h"
#else
#include "StockScreen.h"
#endif

namespace {

class StockApp : public App {
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
  DemoScreen screen;
#else
  StockScreen screen;
#endif

public:
  void setup() override {
    // The app's own settings: registered into the shared schema, persisted in
    // settings.json, editable from the served pages — the app/config/html
    // triangle. Deliberately rendered in TWO skins (stance A', ticket #34):
    // the settings UI's App tab is the free one registration buys, and
    // index.html is a custom skin over the identical contract. The note below
    // names that duplication so it reads as the exhibit it is; an app that
    // brings its own UI calls ConfigStore::suppressAppTab() instead.
    ConfigStore::setAppNote(
        "These render here for free — registering a setting is all it takes. "
        "The landing page shows the other path: a custom UI over the same "
        "values. Apps with their own UI can suppress this tab entirely.");
    ConfigStore::registerColor(SettingSection::App, "col_hour", "Clock hour color", "#ffffff");
    ConfigStore::registerColor(SettingSection::App, "col_min", "Clock minute color", "#ffffff");
    ConfigStore::registerColor(SettingSection::App, "col_colon", "Clock colon color", "#ffffff");
    ConfigStore::registerColor(SettingSection::App, "col_host", "Hostname color", "#ffffff");
    ConfigStore::registerColor(SettingSection::App, "col_ip", "IP address color", "#ffffff");
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
    // The demo screen registers its own "Screen effect" choice: the roster of
    // effects lives in one table in DemoScreen.cpp, and the settings catalog is
    // built from it rather than spelled out a second time here.
    screen.registerSettings();
#endif
    Display::setActive(&screen);
  }

  // Lands in the "app" object of GET /api/status. Only the framebuffer build
  // has frame timings to report; the direct-draw StockScreen has none, and an
  // empty "app" object is a perfectly good answer.
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
  void statusJson(JsonObject out) override { screen.statusJson(out); }
#endif

  void onSystemEvent(SysEvent e) override {
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
    // Park on OtaStarting — the contract every app signs, and here it is not
    // politeness but a hard requirement: the effect roster's pool is the
    // largest allocation in the firmware, and the update needs that room to
    // stream through. Held, a firmware upload fails outright.
    if (e == SysEvent::OtaStarting) {
      screen.park();
      return;
    }
#endif
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
