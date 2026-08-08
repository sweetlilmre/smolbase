#include "Touch.h"
#include "Display.h"
#include "smolbase_config.h"
#include <Arduino.h>

namespace Touch {

static constexpr uint32_t LONG_PRESS_MS = 600;
static uint16_t threshold = 0;
static bool down = false;
static uint32_t downAt = 0;
static bool longFired = false;

void begin() {
  // Untouched boot reading minus a margin; below it = finger present.
  threshold = touchRead(SMOLBASE_PIN_TOUCH) - 120;
}

void loop() {
  bool touched = touchRead(SMOLBASE_PIN_TOUCH) < threshold;
  uint32_t now = millis();
  Screen* s = Display::active();
  if (touched && !down) {
    down = true;
    downAt = now;
    longFired = false;
  } else if (touched && down && !longFired && now - downAt >= LONG_PRESS_MS) {
    longFired = true;
    if (s) s->onLongPress();
  } else if (!touched && down) {
    down = false;
    if (!longFired && s) s->onTap();
  }
}

} // namespace Touch
