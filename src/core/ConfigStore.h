// Settings persistence: one JSON document on LittleFS (SMOLBASE_SETTINGS_PATH).
// Missing file or key is the normal state — every getter carries an in-code default.
// WiFi credentials are NOT here; they live in NVS (see Net).
#pragma once
#include <Arduino.h>

namespace ConfigStore {
bool begin(); // mounts LittleFS and loads the document
String getString(const char* key, const char* def);
int32_t getInt(const char* key, int32_t def);
bool getBool(const char* key, bool def);
void setString(const char* key, const String& v);
void setInt(const char* key, int32_t v);
void setBool(const char* key, bool v);
bool save(); // atomic: temp file + rename; posts SysEvent::SettingsChanged
} // namespace ConfigStore
