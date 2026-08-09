// Settings persistence: one JSON document on LittleFS (SMOLBASE_SETTINGS_PATH).
// Missing file or key is the normal state — every getter carries an in-code default.
// WiFi credentials are NOT here; they live in NVS (see Net).
//
// Settings Schema (extension surface, wayfinder ticket #6): code registers each
// setting (key, type, label, default, range) into a static registry tagged with a
// section — "system" for core settings (registered in begin()), "app" for the
// Consumer's own. The registry drives the served Settings UI: schemaToJson() emits
// schema + current values for the /api/settings endpoints; applyJson() validates
// and stores an incoming value map. Raw get/set access remains underneath for
// anything the schema can't express.
//
// Threading: registration must happen during boot (single-threaded, before the web
// server starts) — typically from App::setup(). Getters/setters/schemaToJson/
// applyJson are mutex-guarded and safe from the httpd task.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum class SettingType : uint8_t { String, Int, Bool };
enum class SettingSection : uint8_t { System, App };

// One registered setting. Keys and labels are stored as pointers: pass string
// literals (or other static-lifetime strings) only. Keys are flat — no nesting.
struct SettingDef {
  const char* key;
  const char* label;
  SettingType type;
  SettingSection section;
  const char* defStr; // String type only
  int32_t defInt;     // Int type only
  bool defBool;       // Bool type only
  int32_t minInt;     // Int type only, inclusive
  int32_t maxInt;     // Int type only, inclusive
};

namespace ConfigStore {
bool begin(); // mounts LittleFS, loads the document, registers system settings

// ---- Schema registration (boot-time only; false = registry full or duplicate key) ----
bool registerString(SettingSection s, const char* key, const char* label, const char* def);
bool registerInt(SettingSection s, const char* key, const char* label, int32_t def,
                 int32_t min, int32_t max);
bool registerBool(SettingSection s, const char* key, const char* label, bool def);

// ---- App-section presentation (stance A', ticket #34) ----
// The note renders at the top of the stock settings page's App tab; the
// suppress flag removes that tab from stock rendering wholesale. Both affect
// rendering ONLY — settings stay registered, persisted, and served over the
// API (custom skins depend on that). Boot-time only, like registration; pass
// static-lifetime strings.
void setAppNote(const char* note);
void suppressAppTab();
const char* appNote();   // nullptr when unset
bool appTabSuppressed();

// ---- Schema introspection ----
size_t settingCount();
const SettingDef& settingAt(size_t i);       // i < settingCount()
const SettingDef* findSetting(const char* key); // nullptr if unregistered

// Serializes the full registry + current values into `out`:
//   {"settings":[{"key","section","type","label","default","value"[,"min","max"]},…]
//    [,"appNote":"…"][,"appTabSuppressed":true]}
// appNote/appTabSuppressed appear only when set — absence means "no note" /
// "render the App tab". This is the payload for GET /api/settings (ticket #14).
void schemaToJson(JsonDocument& out);

// Applies a flat {key: value, …} object: unregistered keys and type mismatches are
// ignored, ints are clamped to [min,max]. Returns true if anything changed.
// Does NOT persist — call save() afterwards. This is the sink for POST /api/settings.
bool applyJson(JsonObjectConst src);

// ---- Typed access. Explicit-default overloads for unregistered/raw keys; ----
// ---- single-argument overloads fall back to the registered schema default. ----
String getString(const char* key, const char* def);
int32_t getInt(const char* key, int32_t def);
bool getBool(const char* key, bool def);
String getString(const char* key); // "" if key unregistered
int32_t getInt(const char* key);   // 0 if key unregistered
bool getBool(const char* key);     // false if key unregistered
void setString(const char* key, const String& v);
void setInt(const char* key, int32_t v);
void setBool(const char* key, bool v);

bool save(); // atomic: temp file + rename; posts SysEvent::SettingsChanged
} // namespace ConfigStore
