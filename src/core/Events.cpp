#include "Events.h"
#include <Arduino.h>

namespace Events {

static QueueHandle_t queue = nullptr;

void begin() { queue = xQueueCreate(16, sizeof(SysEvent)); }

bool post(SysEvent e) {
  if (!queue) return false;
  bool ok = xQueueSend(queue, &e, 0) == pdTRUE;
  // A dropped event is close to unrecoverable (a lost NetworkUp strands the
  // portal); with 16 slots drained every ~2 ms it should never happen — if it
  // does, the log is the only witness.
  if (!ok) Serial.printf("[events] queue full, dropped event %d\n", (int)e);
  return ok;
}

void drain(void (*handler)(SysEvent)) {
  SysEvent e;
  while (queue && xQueueReceive(queue, &e, 0) == pdTRUE) handler(e);
}

} // namespace Events
