#include "AppHost.h"
#include "smolbase_config.h"
#include <Arduino.h>

namespace AppHost {

App& app() {
  static App& a = makeApp();
  return a;
}

void setup() { app().setup(); }

void loop() {
#ifdef SMOLBASE_DEBUG
  uint32_t t0 = millis();
  app().loop();
  uint32_t dt = millis() - t0;
  if (dt > SMOLBASE_LOOP_BUDGET_MS)
    Serial.printf("[smolbase] app.loop() took %lu ms (budget %d ms)\n", dt, SMOLBASE_LOOP_BUDGET_MS);
#else
  app().loop();
#endif
}

} // namespace AppHost
