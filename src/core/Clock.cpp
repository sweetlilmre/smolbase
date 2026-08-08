#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include <Arduino.h>
#include <esp_sntp.h>

namespace Clock {

// Fallback pool when the configured server is slow/unreachable. The primary comes
// from the "ntp" setting; SNTP tries these in order.
static const char* kNtpFallback1 = "time.nist.gov";
static const char* kNtpFallback2 = "time.google.com";

static bool synced = false;

bool isSynced() { return synced; }

static void onSntpSync(struct timeval*) {
  // SNTP task context (core 0) — post only.
  synced = true;
  Events::post(SysEvent::TimeSynced);
}

void begin() {
  // "tz" (POSIX string) and "ntp" are registered by ConfigStore::begin(); "tz_name"
  // is the IANA zone the Settings UI picked from zones.json — stored so the UI can
  // round-trip the dropdown selection. The firmware only ever applies "tz".
  ConfigStore::registerString(SettingSection::System, "tz_name", "Timezone", "Etc/UTC");
}

void sync() {
  // Safe to call repeatedly (NetworkUp after a drop, or a settings change while
  // online): configTzTime stops any running SNTP session, re-applies TZ via
  // setenv/tzset, sets the servers, and restarts SNTP. Default sync interval (1h)
  // is kept. The buffers are static because SNTP keeps the server pointers.
  static String ntp;
  static String tz;
  ntp = ConfigStore::getString("ntp"); // defaults live in the settings schema
  tz = ConfigStore::getString("tz");
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(tz.c_str(), ntp.c_str(), kNtpFallback1, kNtpFallback2);
}

void applyTimezone() {
  String tz = ConfigStore::getString("tz");
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

bool nowLocal(struct tm& out) {
  if (!synced) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

} // namespace Clock
