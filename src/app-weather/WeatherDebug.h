// Flight-readiness debug surface (#74), split out of WeatherData's interface
// (#99). json() is the ONE weather consumer allowed off the main loop: it
// runs on the httpd task (core 0 — see App.h's registerRoutes contract), so
// it copies the shared fields under the fetch mux before serializing.
// Includes the current Reading, per-stage fetch codes, heap trajectory, and
// key PRESENCE — never the key (ADR 0003).
//
// Defined in WeatherData.cpp beside the internal state it reads; this header
// exists so WeatherData.h stays free of ArduinoJson and the debug concern.
#pragma once
#include <ArduinoJson.h>

namespace WeatherDebug {

void json(JsonDocument& out); // core-0-callable

} // namespace WeatherDebug
