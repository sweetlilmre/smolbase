#include "ConfigStore.h"
#include "Events.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace ConfigStore {

static JsonDocument doc;
static SemaphoreHandle_t mutex = nullptr;

struct Guard {
  Guard() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex); }
};

bool begin() {
  mutex = xSemaphoreCreateMutex();
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

String getString(const char* key, const char* def) {
  Guard g;
  return doc[key] | String(def);
}
int32_t getInt(const char* key, int32_t def) {
  Guard g;
  return doc[key] | def;
}
bool getBool(const char* key, bool def) {
  Guard g;
  return doc[key] | def;
}
void setString(const char* key, const String& v) { Guard g; doc[key] = v; }
void setInt(const char* key, int32_t v) { Guard g; doc[key] = v; }
void setBool(const char* key, bool v) { Guard g; doc[key] = v; }

bool save() {
  {
    Guard g;
    LittleFS.mkdir("/config");
    File f = LittleFS.open(SMOLBASE_SETTINGS_PATH ".tmp", "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    LittleFS.remove(SMOLBASE_SETTINGS_PATH);
    if (!LittleFS.rename(SMOLBASE_SETTINGS_PATH ".tmp", SMOLBASE_SETTINGS_PATH)) return false;
  }
  Events::post(SysEvent::SettingsChanged);
  return true;
}

} // namespace ConfigStore
