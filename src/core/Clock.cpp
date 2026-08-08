#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include <Arduino.h>
#include <esp_sntp.h>

namespace Clock {

static bool synced = false;

bool isSynced() { return synced; }

static void onSntpSync(struct timeval*) {
  // SNTP task context (core 0) — post only.
  synced = true;
  Events::post(SysEvent::TimeSynced);
}

void sync() {
  sntp_set_time_sync_notification_cb(onSntpSync);
  String ntp = ConfigStore::getString("ntp", "pool.ntp.org");
  String tz = ConfigStore::getString("tz", "UTC0");
  configTzTime(tz.c_str(), ntp.c_str());
}

void applyTimezone() {
  String tz = ConfigStore::getString("tz", "UTC0");
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

} // namespace Clock
