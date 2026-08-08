// Secret Store (design: ticket #23). Consumer-held opaque values — API keys,
// tokens, webhook URLs — in a dedicated NVS namespace, deliberately OUTSIDE
// the settings schema: nothing here auto-renders, serializes, or appears in
// settings.json / fs images, and values are never readable over HTTP (the web
// surface exposes an existence map only; see Web.cpp).
//
// Honesty note (document, don't hide): this is plain NVS. It protects against
// ACCIDENTAL exposure, not against an attacker with physical flash access —
// real at-rest encryption needs an irreversible eFuse burn (see Espressif's
// flash-encryption guide) and is not a supported path for this template.
//
// Core secrets (WiFi credentials, in Net's namespace) are unreachable through
// this API. Survives fs-OTA; dies with factory reset (full NVS erase).
// Size limits are NVS's own: ~4000 B per value, ~20 KB partition shared with
// WiFi creds. Thread-safe (NVS locks internally); callable from any task.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Secrets {
String get(const char* key);                // "" when absent
bool set(const char* key, const String& v); // false = NVS write failed (full?)
bool clear(const char* key);                // true when removed or already absent
bool has(const char* key);
void listJson(JsonDocument& out); // {"key": true, ...} — the existence map
} // namespace Secrets
