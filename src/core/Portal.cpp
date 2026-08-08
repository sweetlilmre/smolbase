#include "Portal.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

namespace Portal {

static DNSServer* dns = nullptr;
static TaskHandle_t task = nullptr;

static void pump(void*) {
  for (;;) {
    if (dns) dns->processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void begin() {
  if (dns) return;
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());
  xTaskCreatePinnedToCore(pump, "portal_dns", 3072, nullptr, 1, &task, 0);
}

void end() {
  if (task) { vTaskDelete(task); task = nullptr; }
  if (dns) { dns->stop(); delete dns; dns = nullptr; }
}

} // namespace Portal
