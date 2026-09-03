// The single capacitive pad (T9/GPIO32) via driver/touch_sens.h: boot-calibrated
// threshold (averaged untouched readings, percentage margin), debounced edges,
// tap vs long-press classification, dispatched to the active Screen from the
// main loop. Tunables: SMOLBASE_TOUCH_*.
//
// A failed calibration leaves the pad INERT rather than guessing a threshold —
// see Touch.cpp for why the old absolute-margin fallback could not be ported.
#pragma once
#include <cstdint>

namespace Touch {
void begin(); // calibrates against averaged untouched boot readings
void loop();

// Observability for tuning SMOLBASE_TOUCH_DELTA_PCT: the value scale is
// driver-specific, so the live baseline is the only way to pick a margin.
// Both are 0 when calibration failed and the pad is inert.
uint32_t padBaseline();
uint32_t padThreshold();
// The most recent reading. Lets a live touch be measured against the baseline
// instead of guessing the margin: hold the pad and watch this fall.
uint32_t padLast();
} // namespace Touch
