// WiFi state machine (wayfinder ticket #8): boot tries stored creds for
// SMOLBASE_CONNECT_TIMEOUT_MS then falls back to AP mode; runtime drops
// auto-reconnect forever — never AP. Credentials live in NVS only.
// Hostname: the ConfigStore "hostname" setting (sanitized) when non-blank,
// else smolbase-XXXX; it is also the mDNS name and the AP SSID.
#pragma once
#include <Arduino.h> // IPAddress; drops out in phase 6
#include <ArduinoJson.h>
#include <IPAddress.h>
#include <string>

namespace Net {
void begin();
void loop(); // pumps the boot-connect timeout + mDNS lifecycle; cheap when settled
bool isUp();
bool inApMode();
IPAddress ip(); // STA IP or AP IP as appropriate
std::string deviceName(); // effective hostname: "hostname" setting or smolbase-XXXX
void applyHostname(); // re-apply after a settings change (re-registers mDNS live)
int32_t rssi();      // STA link RSSI in dBm; 0 when not connected
std::string ssid();  // joined network name; "" when not connected

// The boot join, for anyone drawing it (the core's WifiJoinScreen). True only
// while stored creds are being tried — it goes false the moment the link comes
// up or the timeout hands over to AP mode, so it is never true at runtime.
bool isJoining();
std::string joiningSsid(); // network being joined; "" unless isJoining()
uint32_t joinElapsedMs(); // since the attempt started; 0 unless isJoining()
bool hasCredentials();
bool saveCredentials(const std::string& ssid, const std::string& pass); // false = NVS write failed; then restartToApply()
void clearCredentials(); // factory reset path
void restartToApply(); // brief delay (lets the HTTP response flush), then Platform::restart()

// WiFi scan for the provisioning portal / settings UI (ticket #13 calls these
// from the httpd task on core 0 — they touch no main-loop state).
void scanNetworks(); // kick an async scan; in AP mode flips to AP_STA so the AP stays up
// Fills {status:"scanning"|"done", networks:[{ssid,rssi,secure}]} — deduplicated
// by SSID (strongest wins), sorted by RSSI descending. A failed/never-started
// scan is auto-restarted and reported as "scanning".
void scanResultsJson(JsonDocument& out);
} // namespace Net
