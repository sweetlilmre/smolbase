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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <cstdio>

// Consumer-loop task. Arduino gave this for free as an implicit 8 KB loopTask
// pinned to core 1; that size is kept exactly, so the port changes nothing that
// could show up as a stack overflow in an App that was fine before. The
// high-water mark is reported as `loopStackFree` by GET /api/status — watch it
// there rather than guessing, since there is no UART on the dev bench.
static constexpr uint32_t kLoopTaskStack = 8192;
// Arduino's loopTask ran at priority 1, above the idle task and below WiFi.
static constexpr UBaseType_t kLoopTaskPriority = 1;
// ADR 0001: all consumer code is single-threaded on core 1. Core 0 carries WiFi,
// the httpd task and the DNS pump.
static constexpr BaseType_t kLoopTaskCore = 1;

static TaskHandle_t s_loopTask = nullptr;

// Declared in Platform.h — see the note there on why this one lives in main.cpp.
uint32_t Platform::loopStackFree() {
  if (!s_loopTask) return 0;
  return uxTaskGetStackHighWaterMark(s_loopTask);
}

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

static void setup() {
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

static void loop() {
  const uint32_t t0 = Platform::millis();
  Events::drain(onSysEvent);
  Net::loop();
  Web::loop(); // brings the listener up as soon as a netif is up — see Web.h
  Clock::loop(); // SNTP re-kick belt; cheap no-op once synced
  Touch::loop();
  Display::tick();
  AppHost::loop();
  Ota::tickRollbackGuard(); // confirm a fresh image after healthy uptime (#76)
  // Timed here, excluding the yield below: this is the whole pass, which is
  // what SMOLBASE_LOOP_BUDGET_MS is about and what sets touch latency.
  // Reported under "loop" by GET /api/status.
  AppHost::recordPass(Platform::millis() - t0);
  Platform::delayMs(2); // yield to the idle task; keeps the WDT fed without busy-spinning
}

static void loopTask(void*) {
  setup();
  for (;;) loop();
}

extern "C" void app_main() {
  // NVS first, and explicitly: Arduino's initArduino() used to do this, and both
  // Net.cpp (WiFi credentials) and Secrets.cpp (API keys) have been relying on
  // it. Without this every setting and credential reads back absent — which
  // presents as "the device forgot everything", not as an error.
  //
  // The erase-and-retry is the standard recovery for a partition the current NVS
  // version cannot read, and is what initArduino() did too. It DOES discard
  // stored credentials; there is no way to keep them in that state, and leaving
  // NVS uninitialised would lose them anyway (plus break every later write).
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    printf("[boot] nvs unreadable (%s) — erasing\n", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  // Everything else runs on core 1, in a task we own. app_main then returns,
  // which deletes the main task and returns its stack to the heap.
  if (xTaskCreatePinnedToCore(loopTask, "smolbase", kLoopTaskStack, nullptr,
                              kLoopTaskPriority, &s_loopTask, kLoopTaskCore) != pdPASS) {
    printf("[boot] FATAL: could not create the smolbase task\n");
    abort();
  }
}
