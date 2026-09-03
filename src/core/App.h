// The extension surface (wayfinder ticket #6). Consumer code lives in src/app/ and
// reaches the system exclusively through these types. Everything here is called on
// core 1 from the main loop (ADR 0001) — except App::registerRoutes handlers, which
// PsychicHttp runs on its own task (core 0).
#pragma once
#include "Events.h"
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>

class PsychicHttpServer;

// A unit of display ownership. The system never repaints between ticks: paint fully
// in onEnter, then update from tick() only when something changed (dirty-flag pattern).
class Screen {
public:
  virtual ~Screen() = default;
  virtual void onEnter(lgfx::LGFX_Device&) {}
  virtual void onExit() {}
  virtual void tick(lgfx::LGFX_Device&) {}
  virtual void onTap() {}
  virtual void onLongPress() {}
};

class App {
public:
  virtual ~App() = default;
  virtual void setup() {} // boot, after all core modules are up
  virtual void loop() {}  // every main-loop pass; stay under SMOLBASE_LOOP_BUDGET_MS
  virtual void registerRoutes(PsychicHttpServer&) {} // runs on the httpd task (core 0)!
  virtual void onSystemEvent(SysEvent) {}

  // Whatever your app wants visible for diagnosis, written into the "app"
  // object of GET /api/status. Called on the httpd task (core 0), like
  // registerRoutes handlers — so copy anything the main loop writes under
  // whatever guard protects it, and keep it short.
  //
  // This exists so an app never needs its own debug endpoint. One URL to curl,
  // one shape to learn, and no risk of a diagnostic route outliving the problem
  // it was added for. Never put a secret's VALUE here (ADR 0003) — presence,
  // yes; the value, no.
  virtual void statusJson(JsonObject) {}
};

// The link-time seam: src/app/ must define this. Missing it = linker error, by design.
App& makeApp();
