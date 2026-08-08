// WiFi state machine (wayfinder ticket #8): boot tries stored creds for
// SMOLBASE_CONNECT_TIMEOUT_MS then falls back to AP mode; runtime drops
// auto-reconnect forever — never AP. Credentials live in NVS only.
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace Net {
void begin();
void loop(); // pumps the boot-connect timeout; cheap when settled
bool isUp();
bool inApMode();
IPAddress ip(); // STA IP or AP IP as appropriate
String deviceName(); // smolbase-XXXX, also hostname + AP SSID
bool hasCredentials();
void saveCredentials(const String& ssid, const String& pass); // then restart to apply
void clearCredentials(); // factory reset path
} // namespace Net
