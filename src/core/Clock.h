// NTP + POSIX timezone. begin() registers the Clock-owned settings (call from setup(),
// after ConfigStore::begin(), before Net::begin()). Call sync() when the network comes
// up or after a settings change while online; applyTimezone() re-applies the TZ alone
// (no network needed). Settings keys: "ntp" (server), "tz" (POSIX string, what the
// firmware applies), "tz_name" (IANA zone name, set by the Settings UI from zones.json
// alongside "tz"; stored for display/round-tripping only — never parsed here).
#pragma once
#include <ctime>

namespace Clock {
void begin(); // registers Clock-owned settings ("tz_name") in the ConfigStore schema
void sync();  // configTzTime with stored NTP server (+ fallbacks) + TZ; posts TimeSynced
void loop();  // main loop: re-kicks SNTP if online-but-unsynced past the window (#38)
void applyTimezone();               // setenv("TZ")/tzset from stored "tz"
bool isSynced();                    // true once SNTP has delivered real time
bool nowLocal(struct tm& out);      // local wall time; false (out untouched) if not synced
} // namespace Clock
