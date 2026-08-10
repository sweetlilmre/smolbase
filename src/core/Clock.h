// NTP + POSIX timezone. begin() is a lifecycle hook (call from setup(), after
// ConfigStore::begin(), before Net::begin()). Call sync() when the network comes
// up or after a settings change while online; applyTimezone() re-applies the TZ
// alone (no network needed). Settings keys: "ntp" (server), "tz" (a Choice
// setting owned by ConfigStore: POSIX value applied here, IANA label persisted
// as "tz_name" for the UI — never parsed by the firmware).
#pragma once
#include <ctime>

namespace Clock {
void begin(); // lifecycle hook (settings are owned by ConfigStore since #57)
void sync();  // configTzTime with stored NTP server (+ fallbacks) + TZ; posts TimeSynced
void loop();  // main loop: re-kicks SNTP if online-but-unsynced past the window (#38)
void applyTimezone();               // setenv("TZ")/tzset from stored "tz"
bool isSynced();                    // true once SNTP has delivered real time
bool nowLocal(struct tm& out);      // local wall time; false (out untouched) if not synced
} // namespace Clock
