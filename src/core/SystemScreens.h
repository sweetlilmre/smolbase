// The system's own screens: AP-mode provisioning info. Owned by the core, shown via
// Display::systemTakeover while provisioning.
#pragma once
#include "App.h"

class ApInfoScreen : public Screen {
public:
  void onEnter(lgfx::LGFX_Device& d) override;
};

ApInfoScreen& apInfoScreen();
