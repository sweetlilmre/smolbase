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

class App;
class PsychicHttpServer;

namespace Web {
// begin() registers everything but does NOT start the listener: PsychicHttp's
// start() hard-fails unless some netif is up with an IP (verified in 3.1.2
// source), and begin() runs before WiFi has one. start() is idempotent and is
// called from the NetworkUp / ApModeEntered event handlers in main.cpp.
void begin(App& app);
void start();
PsychicHttpServer& server(); // advanced/consumer escape hatch
} // namespace Web
