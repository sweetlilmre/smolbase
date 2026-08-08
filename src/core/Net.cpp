#include "Net.h"
#include "Events.h"
#include "smolbase_config.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

namespace Net {

enum class State { Idle, Connecting, Sta, Ap };
static State state = State::Idle;
static uint32_t connectStart = 0;
static String name;

String deviceName() { return name; }
bool isUp() { return state == State::Sta && WiFi.isConnected(); }
bool inApMode() { return state == State::Ap; }
IPAddress ip() { return state == State::Ap ? WiFi.softAPIP() : WiFi.localIP(); }

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

void saveCredentials(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("smolbase", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

void clearCredentials() {
  Preferences p;
  p.begin("smolbase", false);
  p.clear();
  p.end();
}

static void startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(name.c_str()); // open AP; its single page is the provisioning portal
  state = State::Ap;
  Events::post(SysEvent::ApModeEntered);
}

static void onWiFiEvent(WiFiEvent_t event) {
  // Runs on the WiFi task (core 0) — post events only, never touch consumer state.
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      if (state == State::Connecting || state == State::Sta) {
        state = State::Sta;
        Events::post(SysEvent::NetworkUp);
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (state == State::Sta) Events::post(SysEvent::NetworkDown);
      // No runtime AP fallback (ticket #8): auto-reconnect runs forever.
      break;
    default:
      break;
  }
}

void begin() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);
  name = String(SMOLBASE_NAME_PREFIX "-") + suffix;

  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false); // creds live in our own NVS namespace, single source of truth

  String ssid, pass;
  loadCreds(ssid, pass);
  if (ssid.isEmpty()) {
    startAp();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(name.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  state = State::Connecting;
  connectStart = millis();
}

void loop() {
  if (state == State::Connecting && millis() - connectStart > SMOLBASE_CONNECT_TIMEOUT_MS) {
    // Boot-time fallback only: the stored network never answered.
    WiFi.disconnect(true);
    startAp();
  }
  static bool mdnsUp = false;
  if (!mdnsUp && isUp()) mdnsUp = MDNS.begin(name.c_str());
}

} // namespace Net
