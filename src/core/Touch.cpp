#include "Touch.h"
#include "Display.h"
#include "smolbase_config.h"
#include <Arduino.h>

namespace Touch {

static uint16_t threshold = SMOLBASE_TOUCH_DEFAULT_THRESHOLD;

// Raw (undebounced) pad state and when it last changed.
static bool rawState = false;
static uint32_t rawSince = 0;

// Debounced press state and long-press bookkeeping.
static bool down = false;
static uint32_t downAt = 0;
static bool longFired = false;

void begin() {
  // Average several untouched readings — a single sample is noisy on the ESP32
  // touch peripheral and would make the threshold a coin toss.
  uint32_t sum = 0;
  for (int i = 0; i < SMOLBASE_TOUCH_CAL_SAMPLES; ++i) {
    sum += touchRead(SMOLBASE_PIN_TOUCH);
    delay(2);
  }
  uint16_t baseline = sum / SMOLBASE_TOUCH_CAL_SAMPLES;

  // An implausibly low baseline means a finger was on the pad during boot (or the
  // pad is misbehaving); calibrating against it would invert the logic. Use the
  // conservative default instead. Also guards the unsigned subtraction below.
  if (baseline < SMOLBASE_TOUCH_MIN_BASELINE || baseline <= SMOLBASE_TOUCH_MARGIN) {
    threshold = SMOLBASE_TOUCH_DEFAULT_THRESHOLD;
  } else {
    threshold = baseline - SMOLBASE_TOUCH_MARGIN;
  }
}

void loop() {
  uint32_t now = millis();
  bool raw = touchRead(SMOLBASE_PIN_TOUCH) < threshold;

  // Track how long the raw state has been stable.
  if (raw != rawState) {
    rawState = raw;
    rawSince = now;
  }

  // Accept an edge only once the raw state has held for the debounce window,
  // so a single noisy sample can't fire a phantom tap or drop a hold.
  if (rawState != down && now - rawSince >= SMOLBASE_TOUCH_DEBOUNCE_MS) {
    down = rawState;
    if (down) { // press edge: timed from first stable contact
      downAt = rawSince;
      longFired = false;
    } else if (!longFired) { // release edge: short hold = tap
      Screen* s = Display::active();
      if (s) s->onTap();
    }
  }

  // Long-press fires once while still held; the release then emits nothing.
  if (down && !longFired && now - downAt >= SMOLBASE_TOUCH_LONGPRESS_MS) {
    longFired = true;
    Screen* s = Display::active();
    if (s) s->onLongPress();
  }
}

} // namespace Touch
