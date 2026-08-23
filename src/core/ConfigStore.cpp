#include "ConfigStore.h"
#include "Events.h"
#include "Fs.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <esp_littlefs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>

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
  d = SettingDef{key, label, t, s, "", 0, false, 0, 0, "", nullptr, 0, nullptr};
  return &d;
}

// A Choice setting's label persists under this derived key (ADR 0002) —
// "tz" stores the POSIX value, "tz_name" the IANA label the user picked.
static std::string labelKey(const char* key) { return std::string(key) + "_name"; }

// "#RRGGBB", case-insensitive — the exact format <input type="color"> emits.
static bool isHexColor(const char* s) {
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  for (int i = 1; i < 7; ++i)
    if (!isxdigit((unsigned char)s[i])) return false;
  return true;
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

bool registerChoice(SettingSection s, const char* key, const char* label,
                    const char* defLabel, const char* defValue,
                    const SettingChoice* options, uint8_t count) {
  if (!defLabel || !defValue || !options || count == 0) return false;
  SettingDef* d = addEntry(s, SettingType::Choice, key, label);
  if (!d) return false;
  d->defStr = defValue;
  d->defLabel = defLabel;
  d->options = options;
  d->optionCount = count;
  return true;
}

bool registerChoiceUrl(SettingSection s, const char* key, const char* label,
                       const char* defLabel, const char* defValue,
                       const char* optionsUrl) {
  if (!defLabel || !defValue || !optionsUrl) return false;
  SettingDef* d = addEntry(s, SettingType::Choice, key, label);
  if (!d) return false;
  d->defStr = defValue;
  d->defLabel = defLabel;
  d->optionsUrl = optionsUrl;
  return true;
}

bool registerColor(SettingSection s, const char* key, const char* label, const char* def) {
  if (!isHexColor(def)) return false;
  SettingDef* d = addEntry(s, SettingType::Color, key, label);
  if (!d) return false;
  d->defStr = def;
  return true;
}

// App-tab presentation (stance A', ticket #34). Boot-time writes like
// registration; read from the httpd task only via schemaToJson (under Guard).
static const char* appNoteStr = nullptr;
static bool appTabOff = false;

void setAppNote(const char* note) { Guard g; appNoteStr = note; }
void suppressAppTab() { Guard g; appTabOff = true; }
const char* appNote() { Guard g; return appNoteStr; }
bool appTabSuppressed() { Guard g; return appTabOff; }

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
  if (appNoteStr) out["appNote"] = appNoteStr;
  if (appTabOff) out["appTabSuppressed"] = true;
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
      case SettingType::Color:
        o["type"] = "color";
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
      case SettingType::Choice: {
        o["type"] = "choice";
        o["default"] = d.defStr;
        o["defaultLabel"] = d.defLabel;
        o["value"] = doc[d.key] | d.defStr;
        o["valueLabel"] = doc[labelKey(d.key)] | d.defLabel;
        if (d.optionsUrl) {
          o["optionsUrl"] = d.optionsUrl;
        } else {
          JsonObject opts = o["options"].to<JsonObject>();
          for (uint8_t j = 0; j < d.optionCount; ++j)
            opts[d.options[j].label] = d.options[j].value;
        }
        break;
      }
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
      case SettingType::Color: {
        // Like String, but a malformed value is rejected (type mismatch),
        // keeping the store parseable by hexRgb-style consumers.
        if (!v.is<const char*>()) break;
        const char* nv = v.as<const char*>();
        if (!isHexColor(nv)) break;
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
      case SettingType::Choice: {
        // The pair arrives as {key: value, key_name: label}. Inline catalogs
        // are validated and the label is derived HERE (the catalog is
        // authoritative, the posted label ignored); URL catalogs live
        // browser-side, so both halves are trusted as-is — but only as a
        // pair, a value without its label would desync the display.
        if (!v.is<const char*>()) break;
        const char* nv = v.as<const char*>();
        const char* nl = nullptr;
        if (d.options) {
          for (uint8_t j = 0; j < d.optionCount; ++j)
            if (strcmp(d.options[j].value, nv) == 0) { nl = d.options[j].label; break; }
        } else {
          JsonVariantConst lv = src[labelKey(d.key)];
          if (lv.is<const char*>()) nl = lv.as<const char*>();
        }
        if (!nl) break; // not in the catalog / label missing: ignored
        std::string lk = labelKey(d.key);
        const char* curV = doc[d.key] | d.defStr;
        const char* curL = doc[lk] | d.defLabel;
        if (strcmp(curV, nv) != 0 || strcmp(curL, nl) != 0) {
          doc[d.key] = nv;
          doc[lk] = nl;
          changed = true;
        }
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
  // Timezone is the founding Choice setting (ticket #57): the user picks an
  // IANA name, the firmware applies the matching POSIX rule. The catalog is
  // the verbatim upstream zones.json (ticket #5), fetched by the browser —
  // the firmware never parses it. Persists as "tz" (POSIX) + "tz_name" (IANA).
  registerChoiceUrl(SettingSection::System, "tz", "Timezone", "Etc/UTC", "UTC0",
                    "/zones.json");
  registerString(SettingSection::System, "ntp", "NTP server", "pool.ntp.org");
  registerInt(SettingSection::System, "brightness", "Brightness", 200, 0, 255);
  // ".local" is added by mDNS — typing it here would sanitize to "...local".
  registerString(SettingSection::System, "hostname", "Hostname (no .local; blank = auto)", "");
}

bool begin() {
  mutex = xSemaphoreCreateMutex();
  registerSystemSettings();
  // Partition label is "spiffs" (historical, from the stock flash layout) but the
  // filesystem is LittleFS.
  //
  // Mounted at the ROOT (base_path ""), so "/w/index.html.gz" is both the URL
  // suffix and the POSIX path and there is only one spelling of every path in
  // the firmware — see the SMOLBASE_FS_MOUNT note in smolbase_config.h. The
  // phase 0 spike (check 12) proved an empty base_path resolves unprefixed
  // paths against this volume.
  //
  // format_if_mount_failed matches what Arduino's LittleFS.begin(true, ...) did:
  // a virgin or corrupted data partition becomes an empty filesystem rather than
  // a device that cannot store settings at all.
  esp_vfs_littlefs_conf_t fsConf = {};
  fsConf.base_path = SMOLBASE_FS_MOUNT;
  fsConf.partition_label = "spiffs";
  fsConf.format_if_mount_failed = true;
  fsConf.dont_mount = false;
  esp_err_t fsErr = esp_vfs_littlefs_register(&fsConf);
  if (fsErr != ESP_OK) {
    printf("[cfg] littlefs mount failed: %s\n", esp_err_to_name(fsErr));
    return false;
  }
  Fs::File f(SMOLBASE_SETTINGS_FSPATH, "r");
  // Missing file is the normal first-boot state; a parse failure leaves doc
  // empty and the in-code defaults apply. Closes by scope.
  if (f) deserializeJson(doc, f);
  return true;
}

// ---- Typed access ----

std::string getString(const char* key, const char* def) {
  Guard g;
  // Arduino String tolerated a null char*; std::string(nullptr) is UB. defStr
  // is never null in practice (addEntry seeds it to "" and registerString
  // guards), but this is the kind of latent difference that only bites in the
  // field, so make it explicit.
  const char* v = doc[key] | def;
  return v ? std::string(v) : std::string(); // copy: safe after the lock is released
}
int32_t getInt(const char* key, int32_t def) {
  Guard g;
  return doc[key] | def;
}
bool getBool(const char* key, bool def) {
  Guard g;
  return doc[key] | def;
}

std::string getString(const char* key) {
  const SettingDef* d = findSetting(key);
  return getString(key, (d && d->defStr) ? d->defStr : "");
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

void setString(const char* key, const std::string& v) { Guard g; doc[key] = v; }
void setInt(const char* key, int32_t v) { Guard g; doc[key] = v; }
void setBool(const char* key, bool v) { Guard g; doc[key] = v; }

// ---- Persistence ----

bool save() {
  {
    Guard g;
    Fs::mkdir(SMOLBASE_FS_MOUNT "/config");
    size_t written = 0;
    {
      Fs::File f(SMOLBASE_SETTINGS_FSPATH ".tmp", "w");
      if (!f) return false;
      written = serializeJson(doc, f);
    } // closed here, by scope, BEFORE the rename below
    if (written == 0) { // out of space / write error: leave the old file intact
      Fs::remove(SMOLBASE_SETTINGS_FSPATH ".tmp");
      return false;
    }
    // littlefs rename atomically replaces an existing destination, so the settings
    // file is never absent. Fall back to remove+rename in case the VFS refuses.
    if (!Fs::rename(SMOLBASE_SETTINGS_FSPATH ".tmp", SMOLBASE_SETTINGS_FSPATH)) {
      Fs::remove(SMOLBASE_SETTINGS_FSPATH);
      if (!Fs::rename(SMOLBASE_SETTINGS_FSPATH ".tmp", SMOLBASE_SETTINGS_FSPATH)) return false;
    }
  }
  Events::post(SysEvent::SettingsChanged);
  return true;
}

} // namespace ConfigStore
