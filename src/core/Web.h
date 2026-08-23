// PsychicHttp lifecycle and registration ordering (tickets #6, #12).
//
// Registration order in begin() is STRUCTURAL — PsychicHttp dispatches
// endpoints first-registered-first-matched, so earlier entries shadow later
// ones. Do not reorder:
//   1. system API routes   (/api/status, /api/wifi*, /api/settings,
//                           /api/factory-reset)
//   2. Ota::registerRoutes (/api/update — slot; ticket #18 fills it in)
//   3. app.registerRoutes  (consumer routes; may NOT claim /api/* system paths)
//   4. static assets       (LittleFS SMOLBASE_WWW_DIR, gzip-only; "/" is
//                           rewritten to /portal.html while in AP mode)
//   5. captive catch-all   (onNotFound: in AP mode, requests for foreign hosts
//                           are 302-redirected to http://<ap-ip>/ so phone/OS
//                           connectivity probes pop the portal; everything
//                           else is a plain 404)
//
// All handlers run on the httpd task (core 0): keep them small, never touch
// Display/screens from them (post a SysEvent instead).
#pragma once
#include <ArduinoJson.h>
#include <esp_err.h>

class App;
class PsychicHttpServer;
class PsychicResponse;

namespace Web {
// begin() registers everything but does NOT start the listener: PsychicHttp's
// start() hard-fails unless some netif is up with an IP (verified in 3.1.2
// source), and begin() runs before WiFi has one. start() is idempotent and is
// called from the NetworkUp / ApModeEntered event handlers in main.cpp.
void begin(App& app);

// Brings the listener up the moment a netif is genuinely usable, and keeps
// trying until one is. Call every main-loop pass; a no-op once running.
//
// This is the mechanism, not a belt. PsychicHttp's start() requires a netif
// that is BOTH up and holding a non-zero IP (PsychicHttpServer::isConnected ->
// esp_netif_is_netif_up plus a non-zero address), and the events that mean "we
// have a network" do not guarantee either: Net::startAp() posts ApModeEntered
// on the line after esp_wifi_start(), which is asynchronous — the AP netif is
// not up until WIFI_EVENT_AP_START lands on the WiFi task.
//
// A one-shot start() lost that race, and the consequence was as bad as it gets
// on an OTA-only device: boot with no stored credentials (a fresh flash, a
// factory reset, any /api/wifi/forget) and the AP is joinable, DHCP hands out
// leases, the captive DNS responder answers every query — and nothing is
// listening on port 80, so there is no way to provision it and no way back in
// without opening the case. The only trace was a printf to a UART. Diagnosed on
// hardware 2026-08-23: ping to 192.168.4.1 fine, DNS fine, tcp/80 refused.
void loop();

// Immediate attempt, from the NetworkUp / ApModeEntered handlers. Safe to call
// when the precondition does not hold yet — loop() will finish the job.
void start();

// Serialize `doc` and send it as application/json. The one way this firmware
// answers with JSON built from values it does not control: ArduinoJson escapes
// them, hand-concatenation does not. A GitHub tag or an error message with a
// quote in it produces malformed JSON the client cannot parse, and the client
// is the settings page.
esp_err_t sendJson(PsychicResponse* res, int code, const JsonDocument& doc);
PsychicHttpServer& server(); // advanced/consumer escape hatch
} // namespace Web
