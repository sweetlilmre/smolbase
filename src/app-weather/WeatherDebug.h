// Flight-readiness debug surface (#74), split out of WeatherData's interface
// (#99). json() is the ONE weather consumer allowed off the main loop: it runs
// on the httpd task (core 0), so it copies the shared fields under the fetch
// mux before serializing. Includes the current Reading, per-stage fetch codes
// and key PRESENCE — never the key (ADR 0003).
//
// Reached through App::statusJson, so this lands in the "app" object of
// GET /api/status. It used to be its own /api/debug/weather route; one endpoint
// for the whole device is one thing to curl and one shape to learn, and a
// diagnostic route cannot outlive the problem it was added for.
//
// Defined in WeatherData.cpp beside the internal state it reads; this header
// exists so WeatherData.h stays free of ArduinoJson and the debug concern.
#pragma once
#include <ArduinoJson.h>

namespace WeatherDebug {

void json(JsonObject out); // core-0-callable

} // namespace WeatherDebug
