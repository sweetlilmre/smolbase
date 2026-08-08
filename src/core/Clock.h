// NTP + POSIX timezone. Call sync() when the network comes up; applyTimezone() re-applies
// after a settings change without reboot. Settings keys: "ntp" (server), "tz" (POSIX string).
#pragma once

namespace Clock {
void sync(); // configTzTime with stored NTP server + TZ; posts TimeSynced on completion
void applyTimezone();
bool isSynced();
} // namespace Clock
