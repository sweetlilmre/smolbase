// Boot sequence (wayfinder ticket #8):
//   ConfigStore → Display → Touch → Net (STA-or-AP decision) → Web → App.
// The main loop (core 1) drains the event queue, pumps the network state machine,
// samples touch, ticks the active screen, and runs the consumer app — nothing else.
// Core 0 carries WiFi, the httpd task, and the AP-mode DNS pump.
#include "core/AppHost.h"
#include "core/Clock.h"
#include "core/ConfigStore.h"
#include "core/Display.h"
#include "core/Events.h"
#include "core/Net.h"
#include "core/Portal.h"
#include "core/SystemScreens.h"
#include "core/Touch.h"
#include "core/Web.h"
#include <Arduino.h>

static void onSysEvent(SysEvent e) {
  switch (e) {
    case SysEvent::ApModeEntered:
      Portal::begin();
      Display::systemTakeover(&apInfoScreen());
      break;
    case SysEvent::NetworkUp:
      Portal::end();
      Display::systemRelease();
      Clock::sync();
      break;
    case SysEvent::SettingsChanged:
      Clock::applyTimezone();
      Display::setBrightness(ConfigStore::getInt("brightness", 200));
      break;
    default:
      break;
  }
  AppHost::app().onSystemEvent(e);
}

void setup() {
  Serial.begin(115200);
  Events::begin();
  ConfigStore::begin();
  Display::begin();
  Display::setBrightness(ConfigStore::getInt("brightness", 200));
  Touch::begin();
  Net::begin();
  Web::begin(AppHost::app());
  AppHost::setup();
}

void loop() {
  Events::drain(onSysEvent);
  Net::loop();
  Touch::loop();
  Display::tick();
  AppHost::loop();
  delay(2); // yield to the idle task; keeps the WDT fed without busy-spinning
}
