// YOUR APP GOES HERE. This file is the worked example consumers gut and
// replace: thin glue wiring a screen (BoingScreen on framebuffer builds,
// StockScreen otherwise — each in its own file, gut them piecewise) to the
// system through every extension hook the template offers.
#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
#include "BoingScreen.h"
#else
#include "StockScreen.h"
#endif

namespace {

class StockApp : public App {
#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
  BoingScreen screen;
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
    ConfigStore::registerBool(SettingSection::App, "boing", "Boing ball", true);
    screen.begin();
#endif
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
