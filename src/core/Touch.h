// The single capacitive pad (GPIO32): boot-calibrated threshold (averaged, with a
// finger-at-boot fallback), debounced edges, tap vs long-press classification,
// dispatched to the active Screen from the main loop. Tunables: SMOLBASE_TOUCH_*.
#pragma once

namespace Touch {
void begin(); // calibrates against averaged untouched boot readings
void loop();
} // namespace Touch
