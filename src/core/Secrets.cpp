// See Secrets.h for the contract. Raw IDF nvs API (not Preferences) because
// the existence map needs namespace enumeration, which Preferences hides.
#include "Secrets.h"
#include <nvs.h>

namespace {
// The app namespace. Core namespaces (e.g. Net's credentials) are simply
// never opened here — that privacy boundary is the design, not an accident.
constexpr const char* NS = "sb-appsec"; // NVS namespace names cap at 15 chars
} // namespace

namespace Secrets {

String get(const char* key) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return String(""); // namespace not created yet
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) {
    nvs_close(h);
    return String("");
  }
  // Short-lived heap buffer (len includes the NUL, capped by NVS at ~4000 B):
  // too big for a task stack, and a static buffer would race between tasks.
  char* buf = (char*)malloc(len);
  if (!buf) {
    nvs_close(h);
    return String("");
  }
  String out;
  if (nvs_get_str(h, key, buf, &len) == ESP_OK) out = buf;
  nvs_close(h);
  memset(buf, 0, len); // don't leave secret bytes lying around
  free(buf);
  return out;
}

bool set(const char* key, const String& v) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_str(h, key, v.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

bool clear(const char* key) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return true; // nothing stored at all
  esp_err_t e = nvs_erase_key(h, key);
  bool ok = (e == ESP_OK && nvs_commit(h) == ESP_OK) || e == ESP_ERR_NVS_NOT_FOUND;
  nvs_close(h);
  return ok;
}

bool has(const char* key) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 0;
  bool found = nvs_get_str(h, key, nullptr, &len) == ESP_OK;
  nvs_close(h);
  return found;
}

void listJson(JsonDocument& out) {
  out.to<JsonObject>(); // {} even when the namespace is empty/absent
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_STR, &it);
  while (e == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    out[info.key] = true;
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
}

} // namespace Secrets
