#include "Net.h"
#include "ConfigStore.h"
#include "Events.h"
#include "Platform.h"
#include "smolbase_config.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <mdns.h>
#include <nvs.h>
#include <vector>

namespace Net {

enum class State : uint8_t { Idle, Connecting, Sta, Ap };
// Written by the WiFi event task (core 0) and the main loop (core 1).
static std::atomic<State> state{State::Idle};
// The link itself, distinct from `state`: a disconnect leaves state == Sta (the
// reconnect-forever policy never leaves STA) but must make isUp() false, which
// is what WiFi.isConnected() used to provide.
static std::atomic<bool> linkUp{false};
static uint32_t connectStart = 0;
static std::string name;
static std::string joinSsid; // the network begin() is trying; for the boot join screen

// esp_netif handles. Both are created up front so PsychicHttp's ON_AP_FILTER /
// ON_STA_FILTER can classify a request by the netif that carried it — they read
// ESP_NETIF_DHCP_SERVER off the handle, so the AP netif must exist even while
// the AP is down (an inactive netif holds 0.0.0.0 and cannot false-match).
static esp_netif_t* staNetif = nullptr;
static esp_netif_t* apNetif = nullptr;
static std::atomic<uint32_t> staIp{0}; // network byte order, 0 when not up

std::string deviceName() { return name; }
bool isUp() { return state.load() == State::Sta && linkUp.load(); }
bool inApMode() { return state.load() == State::Ap; }

static std::string ipToString(uint32_t addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR((const esp_ip4_addr_t*)&addr));
  return buf;
}

std::string ip() {
  if (inApMode()) {
    esp_netif_ip_info_t info = {};
    if (apNetif && esp_netif_get_ip_info(apNetif, &info) == ESP_OK)
      return ipToString(info.ip.addr);
    return "0.0.0.0";
  }
  return ipToString(staIp.load());
}

// esp_wifi_sta_get_ap_info fails unless the link is actually up, which is the
// same condition the old WiFi.RSSI()/WiFi.SSID() guards enforced.
static bool apInfo(wifi_ap_record_t& out) {
  return isUp() && esp_wifi_sta_get_ap_info(&out) == ESP_OK;
}

int32_t rssi() {
  wifi_ap_record_t rec;
  return apInfo(rec) ? rec.rssi : 0;
}

std::string ssid() {
  wifi_ap_record_t rec;
  if (!apInfo(rec)) return {};
  return std::string(reinterpret_cast<const char*>(rec.ssid));
}

// The joined SSID is empty while the link is still coming up, so the name being
// joined is remembered from begin()'s credential load instead.
bool isJoining() { return state.load() == State::Connecting; }
std::string joiningSsid() { return isJoining() ? joinSsid : std::string(); }
uint32_t joinElapsedMs() { return isJoining() ? Platform::millis() - connectStart : 0; }

// mDNS labels: lowercase alnum + dash, no leading/trailing dash, keep it short.
static std::string sanitizeHostname(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    } else if (c == '-' || c == ' ' || c == '_') {
      if (out.length() && out[out.length() - 1] != '-') out += '-';
    } // anything else is dropped
  }
  if (out.length() > 32) out.resize(32);
  while (!out.empty() && out.back() == '-') out.pop_back();
  while (!out.empty() && out.front() == '-') out.erase(0, 1);
  return out;
}

// Credentials live in this NVS namespace. Arduino's Preferences was a thin
// wrapper over exactly these calls, and the phase 0 spike confirmed ON HARDWARE
// that strings written by Preferences::putString read back through
// nvs_get_str — so fielded devices keep their credentials across this change.
// Secrets.cpp already used the raw API; this matches it.
//
// NOTE for phase 7: nothing here calls nvs_flash_init(). Arduino's
// initArduino() does it at startup, so both this and Secrets.cpp have been
// relying on that. app_main() must call it explicitly once Arduino is gone.
static constexpr const char* NVS_NS = "smolbase";

// Reads a string value, leaving `out` untouched when the key is absent.
static bool nvsGetStr(nvs_handle_t h, const char* key, std::string& out) {
  size_t len = 0; // includes the NUL
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return false;
  std::vector<char> buf(len);
  if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK) return false;
  out.assign(buf.data());
  return true;
}

static void loadCreds(std::string& ssid, std::string& pass) {
  nvs_handle_t h;
  // Namespace absent is the normal first-boot state, not an error.
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  nvsGetStr(h, "ssid", ssid);
  nvsGetStr(h, "pass", pass);
  nvs_close(h);
}

bool hasCredentials() {
  std::string s, p;
  loadCreds(s, p);
  return s.length() > 0;
}

bool saveCredentials(const std::string& ssid, const std::string& pass) {
  // A write failure here (e.g. NVS full of an old firmware's namespaces) must
  // surface: silently failing looks like an unexplained provisioning loop to
  // the user.
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_str(h, "ssid", ssid.c_str()) == ESP_OK &&
            nvs_set_str(h, "pass", pass.c_str()) == ESP_OK &&
            nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

void clearCredentials() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h); // Preferences::clear() erased the namespace, same as this
  nvs_commit(h);
  nvs_close(h);
}

void restartToApply() {
  // 1.5 s: the response leaves the lwIP send buffer in <10 ms on a clean link,
  // but a phone in power-save on a flaky AP link needs an RTO retransmission
  // (~1 s) to actually receive it. Blocking the httpd task here is fine — the
  // device is about to reboot anyway.
  Platform::delayMs(1500);
  Platform::restart();
}

// Effective device name: the sanitized "hostname" setting, else smolbase-XXXX.
static std::string computeName() {
  std::string n = sanitizeHostname(ConfigStore::getString("hostname", ""));
  if (!n.empty()) return n;
  // esp_read_mac works before any netif exists; WiFi.macAddress() at this
  // point returns without writing the buffer (verified in the installed core),
  // which made the identity suffix uninitialized-stack garbage.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);
  return std::string(SMOLBASE_NAME_PREFIX "-") + suffix;
}

// mDNS lifecycle state â€” owned by the main loop (loop()/applyHostname() only).
// Arduino's ESPmDNS was a wrapper over the espressif/mdns component; these are
// the same calls it made, minus the global object.
static bool mdnsUp = false;
static uint32_t mdnsLastTry = 0;

// mdns_free() is safe when nothing was started, which the old MDNS.end() call
// relied on to clear a half-dead responder.
static void mdnsStop() {
  mdns_free();
  mdnsUp = false;
}

static bool mdnsStart(const char* host) {
  if (mdns_init() != ESP_OK) return false;
  if (mdns_hostname_set(host) != ESP_OK) {
    mdns_free();
    return false;
  }
  // Instance name is what shows up in service browsers; hostname is fine.
  mdns_instance_name_set(host);
  if (mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0) != ESP_OK) {
    mdns_free();
    return false;
  }
  return true;
}

// ---- scan state ----
// esp_wifi's scan is fire-and-forget with a completion EVENT, where Arduino's
// scanComplete() polled a count. scanDone is set by the event; the results are
// cached on first read because esp_wifi_scan_get_ap_records consumes them.
static std::atomic<bool> scanDone{false};
static std::atomic<bool> scanRunning{false};
static bool scanCached = false;
struct Hit {
  std::string ssid;
  int32_t rssi;
  bool secure;
};
static std::vector<Hit> scanHits;

static void startAp() {
  // Mode change is legal while the driver is running, which is the case when we
  // arrive here from the boot-join timeout.
  esp_wifi_set_mode(WIFI_MODE_AP);
  wifi_config_t cfg = {};
  snprintf(reinterpret_cast<char*>(cfg.ap.ssid), sizeof(cfg.ap.ssid), "%s", name.c_str());
  cfg.ap.ssid_len = (uint8_t)strnlen(reinterpret_cast<char*>(cfg.ap.ssid), sizeof(cfg.ap.ssid));
  cfg.ap.channel = 1;
  cfg.ap.authmode = WIFI_AUTH_OPEN; // open AP; its single page is the provisioning portal
  cfg.ap.max_connection = 4;
  cfg.ap.beacon_interval = 100;
  esp_wifi_set_config(WIFI_IF_AP, &cfg);
  esp_wifi_start(); // ESP_ERR_WIFI_NOT_STOPPED when already running: fine
  state.store(State::Ap);
  Events::post(SysEvent::ApModeEntered);
}

// Runs on the WiFi/event task (core 0) — post events only, never touch consumer
// state. Same contract the Arduino WiFi.onEvent handler had.
static void onWifiEvent(void*, esp_event_base_t, int32_t id, void*) {
  switch (id) {
    case WIFI_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case WIFI_EVENT_STA_DISCONNECTED: {
      linkUp.store(false);
      staIp.store(0);
      State s = state.load();
      if (s == State::Sta) Events::post(SysEvent::NetworkDown);
      // No runtime AP fallback (ticket #8): reconnect forever. esp_wifi has no
      // auto-reconnect of its own, so this IS the reconnect policy rather than
      // a belt on top of one. Guarded by state so the boot-timeout teardown
      // (state -> Idle before disconnect) and AP mode never fight a stray
      // reconnect.
      if (s == State::Sta || s == State::Connecting) esp_wifi_connect();
      break;
    }
    case WIFI_EVENT_SCAN_DONE:
      scanDone.store(true);
      break;
    default:
      break;
  }
}

static void onIpEvent(void*, esp_event_base_t, int32_t, void* data) {
  auto* e = static_cast<ip_event_got_ip_t*>(data);
  staIp.store(e->ip_info.ip.addr);
  linkUp.store(true);
  // CAS, not load-then-store: a late GOT_IP racing the boot-timeout teardown on
  // core 1 must not overwrite Idle/Ap with Sta (that would wedge
  // inApMode()==false while the softAP is actually running).
  State expected = State::Connecting;
  if (state.compare_exchange_strong(expected, State::Sta) || expected == State::Sta) {
    Events::post(SysEvent::NetworkUp);
  }
}

// Brings up esp_netif + the event loop + the WiFi driver. esp_netif_init and
// esp_event_loop_create_default return ESP_ERR_INVALID_STATE when the Arduino
// core has already run them, which is not an error for us.
static void wifiInit() {
  esp_netif_init();
  esp_event_loop_create_default();
  if (!staNetif) staNetif = esp_netif_create_default_wifi_sta();
  if (!apNetif) apNetif = esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&init);
  // Credentials live in our own NVS namespace, single source of truth — do not
  // let the driver keep a second copy (this is WiFi.persistent(false)).
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr,
                                      nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onIpEvent, nullptr,
                                      nullptr);
}

void applyHostname() {
  // Called on SettingsChanged (main loop). A changed name re-registers mDNS
  // immediately; the DHCP hostname and AP SSID pick it up on the next
  // reconnect/boot (changing those live would mean bouncing the link).
  std::string next = computeName();
  if (next == name) return;
  name = next;
  if (staNetif) esp_netif_set_hostname(staNetif, name.c_str());
  if (mdnsUp) mdnsStop(); // loop() re-registers under the new name within ~1 s
}

void begin() {
  name = computeName();
  wifiInit();

  std::string ssid, pass;
  loadCreds(ssid, pass);
  if (ssid.empty()) {
    startAp();
    return;
  }
  joinSsid = ssid;

  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t cfg = {};
  snprintf(reinterpret_cast<char*>(cfg.sta.ssid), sizeof(cfg.sta.ssid), "%s", ssid.c_str());
  snprintf(reinterpret_cast<char*>(cfg.sta.password), sizeof(cfg.sta.password), "%s",
           pass.c_str());
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  // Hostname must be set before the netif comes up for DHCP to carry it.
  esp_netif_set_hostname(staNetif, name.c_str());
  state.store(State::Connecting);
  connectStart = Platform::millis();
  esp_wifi_start(); // STA_START fires and the handler calls esp_wifi_connect()
}

void loop() {
  if (state.load() == State::Connecting &&
      Platform::millis() - connectStart > SMOLBASE_CONNECT_TIMEOUT_MS) {
    // Boot-time fallback only: the stored network never answered. CAS parks the
    // state so a GOT_IP landing at this exact moment wins the race cleanly
    // (we see Sta and skip the teardown) instead of being overwritten.
    State expected = State::Connecting;
    if (state.compare_exchange_strong(expected, State::Idle)) {
      esp_wifi_disconnect();
      startAp();
    }
  }

  // mDNS lifecycle (main loop only): register once up, tear down on a drop so
  // the reconnect path re-registers cleanly.
  bool up = isUp();
  if (mdnsUp && !up) mdnsStop();
  if (!mdnsUp && up && Platform::millis() - mdnsLastTry > 1000) {
    mdnsLastTry = Platform::millis();
    mdnsStop(); // clears any half-dead responder before retrying
    mdnsUp = mdnsStart(name.c_str());
  }
}

// ---- WiFi scan (called from the httpd task, core 0) ----

void scanNetworks() {
  // Scanning needs the STA interface. In AP mode flip to AP_STA — the softAP
  // interface stays up throughout; we deliberately never flip back, since the
  // provisioning flow ends in restartToApply() anyway.
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (mode == WIFI_MODE_AP) {
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start(); // no-op when running; needed if only the AP was up
  }
  scanDone.store(false);
  scanCached = false;
  wifi_scan_config_t sc = {};
  if (esp_wifi_scan_start(&sc, false /*async*/) != ESP_OK) {
    scanRunning.store(false);
    return;
  }
  scanRunning.store(true);
}

void scanResultsJson(JsonDocument& out) {
  if (scanRunning.load() && !scanDone.load()) {
    out["status"] = "scanning";
    out["networks"].to<JsonArray>();
    return;
  }
  if (!scanRunning.load() && !scanCached) {
    // Never started, or the start failed — kick one so polling self-heals.
    scanNetworks();
    out["status"] = "scanning";
    out["networks"].to<JsonArray>();
    return;
  }

  // esp_wifi_scan_get_ap_records CONSUMES the driver's results, so the first
  // poll after a scan caches them. Repeated polls then stay "done" until a new
  // scan, which is the behaviour the portal's polling loop expects.
  if (!scanCached) {
    scanRunning.store(false);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    scanHits.clear();
    if (n) {
      std::vector<wifi_ap_record_t> recs(n);
      if (esp_wifi_scan_get_ap_records(&n, recs.data()) == ESP_OK) {
        for (uint16_t i = 0; i < n; ++i) {
          std::string s(reinterpret_cast<const char*>(recs[i].ssid));
          if (s.empty()) continue; // hidden networks are unjoinable from the portal
          const int32_t r = recs[i].rssi;
          const bool secure = recs[i].authmode != WIFI_AUTH_OPEN;
          bool merged = false;
          for (auto& h : scanHits) {
            if (h.ssid == s) { // dedupe multi-AP SSIDs, strongest signal wins
              if (r > h.rssi) {
                h.rssi = r;
                h.secure = secure;
              }
              merged = true;
              break;
            }
          }
          if (!merged) scanHits.push_back({s, r, secure});
        }
      }
    }
    std::sort(scanHits.begin(), scanHits.end(),
              [](const Hit& a, const Hit& b) { return a.rssi > b.rssi; });
    scanCached = true;
  }

  out["status"] = "done";
  JsonArray arr = out["networks"].to<JsonArray>();
  for (const auto& h : scanHits) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = h.ssid;
    o["rssi"] = h.rssi;
    o["secure"] = h.secure;
  }
}

} // namespace Net
