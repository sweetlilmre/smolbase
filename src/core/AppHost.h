// Finds the consumer App through the makeApp() link-time seam and drives its lifecycle.
#pragma once
#include "App.h"
#include <cstdint>

namespace AppHost {
App& app();
void setup();
void loop(); // times the App's own loop() slice

// Loop telemetry, reported by GET /api/status under "loop". Two scopes, because
// they answer different questions:
//
//   pass — the WHOLE main-loop iteration (events, net, touch, display, app).
//          This is what SMOLBASE_LOOP_BUDGET_MS is about: it sets touch latency
//          and frame pacing, and it is the number that goes wrong.
//   app  — the App::loop() slice alone, so a consumer can tell whether an
//          overrun is theirs. A screen-driven App does its work in
//          Screen::tick() (called from Display::tick(), inside the pass), so a
//          near-zero app slice next to a large pass is normal, not a bug.
//
// max and overruns are since boot. Recorded unconditionally: this used to be an
// #ifdef-gated printf, which put the one number that says whether an App meets
// its contract behind a rebuild — and out of reach entirely on a bench with no
// serial line.
void recordPass(uint32_t ms); // main.cpp calls this once per iteration
uint32_t passLastMs();
uint32_t passMaxMs();
uint32_t passOverruns();
uint32_t appLastMs();
uint32_t appMaxMs();
} // namespace AppHost
