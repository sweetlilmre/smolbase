#include "ConfigStore.h"
#include "Events.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace ConfigStore {

static JsonDocument doc;
static SemaphoreHandle_t mutex = nullptr;

// Static-capacity registry: no heap, no churn. Registration is boot-time only;
// after boot the registry is read-only, so introspection needs no lock.
static SettingDef registry[SMOLBASE_MAX_SETTINGS];
static size_t registryCount = 0;

struct Guard {
  Guard() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex); }
};

// ---- Schema registration ----

// Lock-free lookup for use by callers that already hold the mutex.
static const SettingDef* findSettingLocked(const char* key) {
  if (!key) return nullptr;
  for (size_t i = 0; i < registryCount; ++i)
    if (strcmp(registry[i].key, key) == 0) return &registry[i];
  return nullptr;
}

// Mutex-guarded: App::setup() (the natural place for consumer registration)
// runs after Web::begin(), so registration can overlap a live httpd task
// hitting schemaToJson/findSetting. The lock makes that window safe.
static SettingDef* addEntry(SettingSection s, SettingType t, const char* key, const char* label) {
  if (!key || !label) return nullptr;
  Guard g;
  if (registryCount >= SMOLBASE_MAX_SETTINGS) return nullptr;
  if (findSettingLocked(key)) return nullptr; // duplicate
  SettingDef& d = registry[registryCount++];
  d = SettingDef{key, label, t, s, "", 0, false, 0, 0};
  return &d;
}

bool registerString(SettingSection s, const char* key, const char* label, const char* def) {
  SettingDef* d = addEntry(s, SettingType::String, key, label);
  if (!d) return false;
  d->defStr = def ? def : "";
  return true;
}

bool registerInt(SettingSection s, const char* key, const char* label, int32_t def,
                 int32_t min, int32_t max) {
  SettingDef* d = addEntry(s, SettingType::Int, key, label);
  if (!d) return false;
  d->defInt = def;
  d->minInt = min;
  d->maxInt = max;
  return true;
}

bool registerBool(SettingSection s, const char* key, const char* label, bool def) {
  SettingDef* d = addEntry(s, SettingType::Bool, key, label);
  if (!d) return false;
  d->defBool = def;
  return true;
}

size_t settingCount() { return registryCount; }

const SettingDef& settingAt(size_t i) { return registry[i]; }

const SettingDef* findSetting(const char* key) {
  Guard g;
  return findSettingLocked(key);
}

// ---- Schema serialization / application ----

static int32_t clampInt(const SettingDef& d, int32_t v) {
  if (v < d.minInt) return d.minInt;
  if (v > d.maxInt) return d.maxInt;
  return v;
}

void schemaToJson(JsonDocument& out) {
  Guard g;
  JsonArray arr = out["settings"].to<JsonArray>();
  for (size_t i = 0; i < registryCount; ++i) {
    const SettingDef& d = registry[i];
    JsonObject o = arr.add<JsonObject>();
    o["key"] = d.key;
    o["section"] = d.section == SettingSection::System ? "system" : "app";
    o["label"] = d.label;
    switch (d.type) {
      case SettingType::String:
        o["type"] = "string";
        o["default"] = d.defStr;
        o["value"] = doc[d.key] | d.defStr;
        break;
      case SettingType::Int:
        o["type"] = "int";
        o["default"] = d.defInt;
        o["min"] = d.minInt;
        o["max"] = d.maxInt;
        o["value"] = doc[d.key] | d.defInt;
        break;
      case SettingType::Bool:
        o["type"] = "bool";
        o["default"] = d.defBool;
        o["value"] = doc[d.key] | d.defBool;
        break;
    }
  }
}

bool applyJson(JsonObjectConst src) {
  Guard g;
  bool changed = false;
  for (size_t i = 0; i < registryCount; ++i) {
    const SettingDef& d = registry[i];
    JsonVariantConst v = src[d.key];
    if (v.isNull()) continue;
    switch (d.type) {
      case SettingType::String: {
        if (!v.is<const char*>()) break;
        const char* nv = v.as<const char*>();
        const char* cur = doc[d.key] | d.defStr;
        if (strcmp(cur, nv) != 0) { doc[d.key] = nv; changed = true; }
        break;
      }
      case SettingType::Int: {
        if (!v.is<int32_t>()) break;
        int32_t nv = clampInt(d, v.as<int32_t>());
        if ((doc[d.key] | d.defInt) != nv) { doc[d.key] = nv; changed = true; }
        break;
      }
      case SettingType::Bool: {
        if (!v.is<bool>()) break;
        bool nv = v.as<bool>();
        if ((doc[d.key] | d.defBool) != nv) { doc[d.key] = nv; changed = true; }
        break;
      }
    }
  }
  return changed;
}

// ---- Lifecycle ----

// The core's own settings. Section "system" renders above the Consumer's "app"
// section in the served Settings UI. Blank hostname = auto (smolbase-XXXX).
static void registerSystemSettings() {
  registerString(SettingSection::System, "tz", "Timezone (POSIX TZ)", "UTC0");
  registerString(SettingSection::System, "ntp", "NTP server", "pool.ntp.org");
  registerInt(SettingSection::System, "brightness", "Brightness", 200, 0, 255);
  registerString(SettingSection::System, "hostname", "Hostname (blank = auto)", "");
}

bool begin() {
  mutex = xSemaphoreCreateMutex();
  registerSystemSettings();
  // Partition label is "spiffs" (historical, from the stock flash layout) but the
  // filesystem is LittleFS.
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) return false;
  File f = LittleFS.open(SMOLBASE_SETTINGS_PATH, "r");
  if (f) {
    deserializeJson(doc, f); // parse failure leaves doc empty — defaults apply
    f.close();
  }
  return true;
}

// ---- Typed access ----

String getString(const char* key, const char* def) {
  Guard g;
  return String(doc[key] | def); // copy: safe after the lock is released
}
int32_t getInt(const char* key, int32_t def) {
  Guard g;
  return doc[key] | def;
}
bool getBool(const char* key, bool def) {
  Guard g;
  return doc[key] | def;
}

String getString(const char* key) {
  const SettingDef* d = findSetting(key);
  return getString(key, d ? d->defStr : "");
}
int32_t getInt(const char* key) {
  const SettingDef* d = findSetting(key);
  if (!d) return 0;
  // Clamp to the registered range: applyJson clamps API writes, but a raw
  // setInt() or a pre-existing settings file bypasses it — and e.g. an
  // out-of-range "brightness" (active-LOW PWM) renders the screen unreadable
  // right when the user needs the AP-info screen.
  int32_t v = getInt(key, d->defInt);
  if (d->type == SettingType::Int) v = clampInt(*d, v);
  return v;
}
bool getBool(const char* key) {
  const SettingDef* d = findSetting(key);
  return getBool(key, d ? d->defBool : false);
}

void setString(const char* key, const String& v) { Guard g; doc[key] = v; }
void setInt(const char* key, int32_t v) { Guard g; doc[key] = v; }
void setBool(const char* key, bool v) { Guard g; doc[key] = v; }

// ---- Persistence ----

bool save() {
  {
    Guard g;
    LittleFS.mkdir("/config");
    File f = LittleFS.open(SMOLBASE_SETTINGS_PATH ".tmp", "w");
    if (!f) return false;
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) { // out of space / write error: leave the old file intact
      LittleFS.remove(SMOLBASE_SETTINGS_PATH ".tmp");
      return false;
    }
    // littlefs rename atomically replaces an existing destination, so the settings
    // file is never absent. Fall back to remove+rename in case the VFS refuses.
    if (!LittleFS.rename(SMOLBASE_SETTINGS_PATH ".tmp", SMOLBASE_SETTINGS_PATH)) {
      LittleFS.remove(SMOLBASE_SETTINGS_PATH);
      if (!LittleFS.rename(SMOLBASE_SETTINGS_PATH ".tmp", SMOLBASE_SETTINGS_PATH)) return false;
    }
  }
  Events::post(SysEvent::SettingsChanged);
  return true;
}

} // namespace ConfigStore
