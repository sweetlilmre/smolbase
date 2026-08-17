// The system's own screens: the boot Wi-Fi join, and AP-mode provisioning info.
// Owned by the core, shown via Display::systemTakeover; both draw direct to the
// device so they render identically in every SMOLBASE_FRAMEBUFFER mode.
#pragma once
#include "App.h"

class ApInfoScreen : public Screen {
public:
  void onEnter(lgfx::LGFX_Device& d) override;
};

ApInfoScreen& apInfoScreen();

// Shown from setup() until the link comes up (NetworkUp releases the takeover)
// or the boot timeout falls through to AP mode, where apInfoScreen() replaces
// it. Deliberately boot-only: a runtime drop reconnects in the background and
// is the app's business to show, not the core's to cover the panel for.
class WifiJoinScreen : public Screen {
  uint32_t lastPaintMs = 0; // throttles the animated part; chrome is painted once
  int barFilled = 0;        // px of progress bar already drawn, so growth is a sliver
  int lastSecs = -1;        // countdown value on screen; skips redundant repaints
public:
  void onEnter(lgfx::LGFX_Device& d) override;
  void tick(lgfx::LGFX_Device& d) override;
};

WifiJoinScreen& wifiJoinScreen();
