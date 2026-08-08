// LovyanGFX device (ticket #3 known-good config) + the Screen slot (ticket #6):
// one active Screen; the system may take the slot over (AP mode) and restores the
// consumer's screen afterwards.
#pragma once
#include "App.h"

namespace Display {
void begin();
lgfx::LGFX_Device& gfx();

void setActive(Screen* s);         // consumer-facing slot
void systemTakeover(Screen* s);    // core only: AP-info screen etc.
void systemRelease();              // restore the consumer's screen
Screen* active();                  // whoever owns the panel right now

void tick();                       // main loop: dispatch to the owning screen
void setBrightness(uint8_t level); // 0-255
} // namespace Display
