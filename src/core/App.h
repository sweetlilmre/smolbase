// The extension surface (wayfinder ticket #6). Consumer code lives in src/app/ and
// reaches the system exclusively through these types. Everything here is called on
// core 1 from the main loop (ADR 0001) — except App::registerRoutes handlers, which
// PsychicHttp runs on its own task (core 0).
#pragma once
#include "Events.h"
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
};

// The link-time seam: src/app/ must define this. Missing it = linker error, by design.
App& makeApp();
