// Boot sequence (wayfinder ticket #8):
//   ConfigStore → Display → Touch → Net (STA-or-AP decision) → Web → App.
// The main loop (core 1) drains the event queue, pumps the network state machine,
// samples touch, ticks the active screen, and runs the consumer app — nothing else.
// Core 0 carries WiFi, the httpd task, and the AP-mode DNS pump.
#include "core/AppHost.h"
#include "core/AssetUpdate.h"
#include "core/Clock.h"
#include "core/ConfigStore.h"
#include "core/Display.h"
#include "core/Events.h"
#include "core/Net.h"
#include "core/Ota.h"
#include "core/Platform.h"
#include "core/Portal.h"
#include "core/SystemScreens.h"
#include "core/Touch.h"
#include "core/Web.h"
#include <Arduino.h>

static void onSysEvent(SysEvent e) {
  switch (e) {
    case SysEvent::ApModeEntered:
      Web::start(); // netif has an IP now — see Web.h for why start is deferred
      Portal::begin();
      Display::systemTakeover(&apInfoScreen());
      break;
    case SysEvent::NetworkUp:
      Web::start();
      Portal::end();
      Display::systemRelease();
      Clock::sync();
      break;
    case SysEvent::SettingsChanged:
      Net::applyHostname();
      if (Net::isUp())
        Clock::sync(); // re-applies TZ and re-kicks SNTP (NTP server may have changed)
      else
        Clock::applyTimezone();
      Display::setBrightness(ConfigStore::getInt("brightness"));
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
  // Heal /w before anything serves it: a /w.<ver> backup matching the running
  // version means we were rolled back (or an update tore) — restore it (#122).
  AssetUpdate::bootHeal();
  Display::begin();
  Display::setBrightness(ConfigStore::getInt("brightness")); // schema default
  Touch::begin();
  Clock::begin(); // registers Clock-owned settings; before Net so schema is complete
  Net::begin();
  // Cover the boot join before the app can paint (Display::setActive is a no-op
  // while the system holds the slot, so AppHost::setup below still registers its
  // screen — it just does not reach the panel yet). Released by NetworkUp, or
  // replaced by apInfoScreen if the join times out into AP mode. Net::begin()
  // has already raised the AP when there are no stored credentials, so
  // isJoining() is the whole condition.
  if (Net::isJoining()) Display::systemTakeover(&wifiJoinScreen());
  Web::begin(AppHost::app());
  AppHost::setup();
}

void loop() {
  Events::drain(onSysEvent);
  Net::loop();
  Clock::loop(); // SNTP re-kick belt; cheap no-op once synced
  Touch::loop();
  Display::tick();
  AppHost::loop();
  Ota::tickRollbackGuard(); // confirm a fresh image after healthy uptime (#76)
  Platform::delayMs(2); // yield to the idle task; keeps the WDT fed without busy-spinning
}
