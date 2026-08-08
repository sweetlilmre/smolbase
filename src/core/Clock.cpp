#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include <Arduino.h>
#include <atomic>
#include <esp_sntp.h>

namespace Clock {

// Fallback pool when the configured server is slow/unreachable. The primary comes
// from the "ntp" setting; SNTP tries these in order.
static const char* kNtpFallback1 = "time.nist.gov";
static const char* kNtpFallback2 = "time.google.com";

static std::atomic<bool> synced{false}; // written on the SNTP task, read on core 1

bool isSynced() { return synced.load(); }

static void onSntpSync(struct timeval*) {
  // SNTP task context (core 0) — post only.
  synced.store(true);
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
  // setenv/tzset, sets the servers, and restarts SNTP. Default sync interval
  // (1h) is kept.
  //
  // SNTP stores the RAW server pointer, so the buffer must both outlive the
  // call and never be reallocated: a static String's reassignment frees the old
  // buffer while the SNTP task (core 0) may still be dereferencing it — a real
  // use-after-free window. Fixed char buffers sidestep it; SNTP is also stopped
  // BEFORE the copy so nothing reads mid-overwrite.
  static char ntpBuf[64];
  static char tzBuf[64];
  String ntp = ConfigStore::getString("ntp"); // defaults live in the settings schema
  String tz = ConfigStore::getString("tz");

  // Skip the stop/restart churn (and the NTP-pool hammering) when nothing
  // that SNTP consumes actually changed — every settings save lands here.
  static bool everSynced = false;
  if (everSynced && ntp.equals(ntpBuf) && tz.equals(tzBuf)) return;
  everSynced = true;

  esp_sntp_stop();
  strlcpy(ntpBuf, ntp.c_str(), sizeof(ntpBuf));
  strlcpy(tzBuf, tz.c_str(), sizeof(tzBuf));
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(tzBuf, ntpBuf, kNtpFallback1, kNtpFallback2);
}

void applyTimezone() {
  String tz = ConfigStore::getString("tz");
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

bool nowLocal(struct tm& out) {
  if (!synced.load()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

} // namespace Clock
