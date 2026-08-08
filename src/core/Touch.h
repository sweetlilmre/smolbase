// The single capacitive pad (GPIO32): boot-calibrated threshold, tap vs long-press,
// dispatched to the active Screen from the main loop.
#pragma once

namespace Touch {
void begin(); // calibrates against the untouched boot reading
void loop();
} // namespace Touch
