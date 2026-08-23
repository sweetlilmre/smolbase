#include "AppHost.h"
#include "Platform.h"
#include "smolbase_config.h"

namespace AppHost {

// See AppHost.h for why both scopes exist and why they are always measured.
static uint32_t s_passLast = 0, s_passMax = 0, s_passOverruns = 0;
static uint32_t s_appLast = 0, s_appMax = 0;

App& app() {
  static App& a = makeApp();
  return a;
}

void setup() { app().setup(); }

void loop() {
  const uint32_t t0 = Platform::millis();
  app().loop();
  s_appLast = Platform::millis() - t0;
  if (s_appLast > s_appMax) s_appMax = s_appLast;
}

void recordPass(uint32_t ms) {
  s_passLast = ms;
  if (ms > s_passMax) s_passMax = ms;
  if (ms > SMOLBASE_LOOP_BUDGET_MS) ++s_passOverruns;
}

uint32_t passLastMs() { return s_passLast; }
uint32_t passMaxMs() { return s_passMax; }
uint32_t passOverruns() { return s_passOverruns; }
uint32_t appLastMs() { return s_appLast; }
uint32_t appMaxMs() { return s_appMax; }

} // namespace AppHost
