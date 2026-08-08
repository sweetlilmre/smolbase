#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include "Net.h"
#include "smolbase_config.h"
#include <Arduino.h>
#include <atomic>
#include <esp_sntp.h>

namespace Clock {

// Fallback pool when the configured server is slow/unreachable. The primary comes
// from the "ntp" setting; SNTP tries these in order.
static const char* kNtpFallback1 = "time.nist.gov";
static const char* kNtpFallback2 = "time.google.com";

static std::atomic<bool> synced{false}; // written on the SNTP task, read on core 1

// SNTP stores RAW server pointers, so these must outlive every call and never
// be reallocated (a static String's reassignment frees the old buffer while
// the SNTP task may still dereference it — a real use-after-free window).
static char ntpBuf[64];
static char tzBuf[64];
static bool kicked = false;    // sync() has started SNTP at least once
static uint32_t waitMs = 0;    // unsynced-while-online stopwatch (ticket #38)

bool isSynced() { return synced.load(); }

static void onSntpSync(struct timeval*) {
  // SNTP task context (core 0) — post only.
  synced.store(true);
  Events::post(SysEvent::TimeSynced);
}

// (Re)start SNTP against the buffered server/TZ. SNTP is stopped first so
// nothing reads the buffers mid-overwrite in sync().
static void kick() {
  esp_sntp_stop();
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(tzBuf, ntpBuf, kNtpFallback1, kNtpFallback2);
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
  String ntp = ConfigStore::getString("ntp"); // defaults live in the settings schema
  String tz = ConfigStore::getString("tz");

  // Skip the stop/restart churn (and the NTP-pool hammering) when nothing
  // that SNTP consumes actually changed — every settings save lands here.
  if (kicked && ntp.equals(ntpBuf) && tz.equals(tzBuf)) return;
  kicked = true;

  esp_sntp_stop(); // stopped BEFORE the copy so nothing reads mid-overwrite
  strlcpy(ntpBuf, ntp.c_str(), sizeof(ntpBuf));
  strlcpy(tzBuf, tz.c_str(), sizeof(tzBuf));
  kick();
}

void loop() {
  // Re-kick belt (ticket #38): SNTP occasionally never delivers after a soft
  // restart (observed on-device; lwip's own retries notwithstanding), leaving
  // the screen on "--:--" until a power cycle. If we're online, SNTP has been
  // started, and no sync has landed within the window, restart the session —
  // converts a silent stall into at most a one-window outage.
  if (synced.load() || !kicked || !Net::isUp()) {
    waitMs = 0;
    return;
  }
  uint32_t now = millis();
  if (waitMs == 0) {
    waitMs = now;
    return;
  }
  if (now - waitMs < SMOLBASE_SNTP_REKICK_MS) return;
  waitMs = now;
  Serial.println("[clock] no NTP sync within the re-kick window; restarting SNTP");
  kick();
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
