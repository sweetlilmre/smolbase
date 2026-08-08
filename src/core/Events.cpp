#include "Events.h"
#include <Arduino.h>

namespace Events {

static QueueHandle_t queue = nullptr;

void begin() { queue = xQueueCreate(16, sizeof(SysEvent)); }

bool post(SysEvent e) {
  if (!queue) return false;
  return xQueueSend(queue, &e, 0) == pdTRUE;
}

void drain(void (*handler)(SysEvent)) {
  SysEvent e;
  while (queue && xQueueReceive(queue, &e, 0) == pdTRUE) handler(e);
}

} // namespace Events
