#include "Net.h"
#include "ConfigStore.h"
#include "Events.h"
#include "smolbase_config.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <vector>

namespace Net {

enum class State : uint8_t { Idle, Connecting, Sta, Ap };
// Written by the WiFi event task (core 0) and the main loop (core 1).
static std::atomic<State> state{State::Idle};
static uint32_t connectStart = 0;
static String name;

String deviceName() { return name; }
bool isUp() { return state.load() == State::Sta && WiFi.isConnected(); }
bool inApMode() { return state.load() == State::Ap; }
IPAddress ip() { return inApMode() ? WiFi.softAPIP() : WiFi.localIP(); }
int32_t rssi() { return isUp() ? WiFi.RSSI() : 0; }
String ssid() { return isUp() ? WiFi.SSID() : String(""); }

// mDNS labels: lowercase alnum + dash, no leading/trailing dash, keep it short.
static String sanitizeHostname(const String& raw) {
  String out;
  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    } else if (c == '-' || c == ' ' || c == '_') {
      if (out.length() && out[out.length() - 1] != '-') out += '-';
    } // anything else is dropped
  }
  if (out.length() > 32) out.remove(32);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  while (out.length() && out[0] == '-') out.remove(0, 1);
  return out;
}

static void loadCreds(String& ssid, String& pass) {
  Preferences p;
  p.begin("smolbase", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
}

bool hasCredentials() {
  String s, p;
  loadCreds(s, p);
  return s.length() > 0;
}

bool saveCredentials(const String& ssid, const String& pass) {
  // putString returns bytes written; 0 = failure (e.g. NVS full of the old
  // firmware's namespaces). Surfacing this matters: a silent failure here looks
  // like an unexplained provisioning loop to the user.
  Preferences p;
  if (!p.begin("smolbase", false)) return false;
  bool ok = p.putString("ssid", ssid) == ssid.length() &&
            p.putString("pass", pass) == pass.length();
  p.end();
  return ok;
}

void clearCredentials() {
  Preferences p;
  p.begin("smolbase", false);
  p.clear();
  p.end();
}

void restartToApply() {
  // 1.5 s: the response leaves the lwIP send buffer in <10 ms on a clean link,
  // but a phone in power-save on a flaky AP link needs an RTO retransmission
  // (~1 s) to actually receive it. Blocking the httpd task here is fine — the
  // device is about to reboot anyway.
  delay(1500);
  ESP.restart();
}

static void startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(name.c_str()); // open AP; its single page is the provisioning portal
  state.store(State::Ap);
  Events::post(SysEvent::ApModeEntered);
}

static void onWiFiEvent(WiFiEvent_t event) {
  // Runs on the WiFi task (core 0) — post events only, never touch consumer state.
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      // CAS, not load-then-store: a late GOT_IP racing the boot-timeout
      // teardown on core 1 must not overwrite Idle/Ap with Sta (that would
      // wedge inApMode()==false while the softAP is actually running).
      State expected = State::Connecting;
      if (state.compare_exchange_strong(expected, State::Sta) || expected == State::Sta) {
        Events::post(SysEvent::NetworkUp);
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      State s = state.load();
      if (s == State::Sta) Events::post(SysEvent::NetworkDown);
      // No runtime AP fallback (ticket #8): reconnect forever. Belt-and-braces
      // under arduino-esp32 3.x — the core's auto-reconnect does not fire for
      // every disconnect reason, so kick an explicit reconnect ourselves.
      // Guarded by state so the boot-timeout teardown (state -> Idle before
      // disconnect) and AP mode never fight a stray reconnect.
      if (s == State::Sta || s == State::Connecting) WiFi.reconnect();
      break;
    }
    default:
      break;
  }
}

// Effective device name: the sanitized "hostname" setting, else smolbase-XXXX.
static String computeName() {
  String n = sanitizeHostname(ConfigStore::getString("hostname", ""));
  if (!n.isEmpty()) return n;
  // esp_read_mac works before any netif exists; WiFi.macAddress() at this
  // point returns without writing the buffer (verified in the installed core),
  // which made the identity suffix uninitialized-stack garbage.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);
  return String(SMOLBASE_NAME_PREFIX "-") + suffix;
}

// mDNS lifecycle state — owned by the main loop (loop()/applyHostname() only).
static bool mdnsUp = false;
static uint32_t mdnsLastTry = 0;

void applyHostname() {
  // Called on SettingsChanged (main loop). A changed name re-registers mDNS
  // immediately; the DHCP hostname and AP SSID pick it up on the next
  // reconnect/boot (changing those live would mean bouncing the link).
  String next = computeName();
  if (next == name) return;
  name = next;
  WiFi.setHostname(name.c_str());
  if (mdnsUp) {
    MDNS.end();
    mdnsUp = false; // loop() re-registers under the new name within ~1 s
  }
}

void begin() {
  name = computeName();

  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false); // creds live in our own NVS namespace, single source of truth

  String ssid, pass;
  loadCreds(ssid, pass);
  if (ssid.isEmpty()) {
    startAp();
    return;
  }
  WiFi.setHostname(name.c_str()); // must precede mode(): applies as the STA netif comes up
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  state.store(State::Connecting);
  connectStart = millis();
}

void loop() {
  if (state.load() == State::Connecting && millis() - connectStart > SMOLBASE_CONNECT_TIMEOUT_MS) {
    // Boot-time fallback only: the stored network never answered. CAS parks the
    // state so a GOT_IP landing at this exact moment wins the race cleanly
    // (we see Sta and skip the teardown) instead of being overwritten.
    State expected = State::Connecting;
    if (state.compare_exchange_strong(expected, State::Idle)) {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(true);
      startAp();
    }
  }

  // mDNS lifecycle (main loop only): register once up, tear down on a drop so
  // the reconnect path re-registers cleanly.
  bool up = isUp();
  if (mdnsUp && !up) {
    MDNS.end();
    mdnsUp = false;
  }
  if (!mdnsUp && up && millis() - mdnsLastTry > 1000) {
    mdnsLastTry = millis();
    MDNS.end(); // safe when not started; clears any half-dead responder
    if (MDNS.begin(name.c_str())) {
      MDNS.addService("http", "tcp", 80);
      mdnsUp = true;
    }
  }
}

// ---- WiFi scan (called from the httpd task, core 0) ----

void scanNetworks() {
  // Scanning needs the STA interface. In AP mode flip to AP_STA — the softAP
  // interface stays up throughout; we deliberately never flip back, since the
  // provisioning flow ends in restartToApply() anyway.
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);
  WiFi.scanNetworks(true /*async*/);
}

void scanResultsJson(JsonDocument& out) {
  int16_t n = WiFi.scanComplete();
  if (n < 0) {
    // WIFI_SCAN_RUNNING, or failed/never started — restart so polling self-heals.
    if (n != WIFI_SCAN_RUNNING) scanNetworks();
    out["status"] = "scanning";
    out["networks"].to<JsonArray>();
    return;
  }
  struct Hit {
    String ssid;
    int32_t rssi;
    bool secure;
  };
  std::vector<Hit> hits;
  hits.reserve(n);
  for (int16_t i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue; // hidden networks are unjoinable from the portal
    int32_t rssi = WiFi.RSSI(i);
    bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    bool merged = false;
    for (auto& h : hits) {
      if (h.ssid == ssid) { // dedupe multi-AP SSIDs, strongest signal wins
        if (rssi > h.rssi) {
          h.rssi = rssi;
          h.secure = secure;
        }
        merged = true;
        break;
      }
    }
    if (!merged) hits.push_back({ssid, rssi, secure});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rssi > b.rssi; });

  out["status"] = "done";
  JsonArray arr = out["networks"].to<JsonArray>();
  for (const auto& h : hits) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = h.ssid;
    o["rssi"] = h.rssi;
    o["secure"] = h.secure;
  }
  // Results are left in place so repeated polls stay "done" until a new scan.
}

} // namespace Net
