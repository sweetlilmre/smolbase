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
#include <ArduinoJson.h>
#include <string>

namespace Secrets {
std::string get(const char* key);                // "" when absent
bool set(const char* key, const std::string& v); // false = NVS write failed (full?)
bool clear(const char* key);                // true when removed or already absent
bool has(const char* key);

// Secret Descriptor (ADR 0003): an App declares a secret it consumes — key,
// label, hint — and registration buys UI: the stock Settings UI renders one
// write-only input per descriptor. Call from App::setup with string literals
// (pointers are kept, not copied). Values are NEVER part of a descriptor.
void describe(const char* key, const char* label, const char* hint = nullptr);

// {"key": {"label":..., "hint":..., "set":bool}, ...} — descriptors with
// their set-flags, plus any set-but-undeclared key as {"set":true} (reachable
// by curl, invisible to the panel). Still never values (ADR 0003).
void listJson(JsonDocument& out);
} // namespace Secrets
