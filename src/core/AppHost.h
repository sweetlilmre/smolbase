// Finds the consumer App through the makeApp() link-time seam and drives its lifecycle.
#pragma once
#include "App.h"

namespace AppHost {
App& app();
void setup();
void loop(); // includes the debug-build loop-budget watchdog
} // namespace AppHost
