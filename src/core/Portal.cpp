#include "Portal.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

namespace Portal {

// arduino-esp32 3.x's DNSServer is AsyncUDP-based: start() installs an onPacket
// hijack that runs on the lwIP tcpip thread, and processNextRequest() is an
// empty inline kept for API compatibility. No pump task is needed (an earlier
// revision had one — it was dead weight plus a cross-core vTaskDelete hazard).
static DNSServer* dns = nullptr;

void begin() {
  if (dns) return;
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());
}

void end() {
  if (!dns) return;
  dns->stop(); // synchronous tcpip_api_call; safe to delete afterwards
  delete dns;
  dns = nullptr;
}

} // namespace Portal
