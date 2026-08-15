// The demo screen (PALETTE_8 framebuffer builds only): the device's identity
// (time, hostname, IP) over a rotation of full-screen 256-color effects, one
// long press apart. The screen owns the clock overlay, the 30 Hz timestep and
// the roster; the effects own the pixels underneath and their half of the
// palette (see effects/Effect.h for the split). Adding a sixth effect is one
// file under effects/ and one line in the roster table.
#pragma once
#include "../core/App.h"
#include "../core/Display.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class DemoScreen : public Screen {
  bool dirty = true;
  bool entered = false;
  bool parked = false; // an OTA is streaming: hold the calm clock, stay freed
  int idx = 0; // index into the roster (see DemoScreen.cpp)
  int lastMinute = -1;
  bool colonOn = true;
  uint32_t lastFrameMs = 0;
  uint32_t nameShownMs = 0; // the "now showing" banner after a switch
  uint32_t colHour = 0xffffff, colMin = 0xffffff, colColon = 0xffffff,
           colHost = 0xffffff, colIp = 0xffffff;

  void applySettings(lgfx::LGFX_Sprite& f);
  void select(int i, lgfx::LGFX_Sprite& f);
  bool calmDue() const;
  void drawIdentity(lgfx::LGFX_Sprite& f);
  static void shadowString(lgfx::LGFX_Sprite& f, const String& s, int x, int y,
                           uint8_t idx);

public:
  void markDirty() { dirty = true; }

  // Registers the "effect" choice setting. The roster is the only place the
  // effect list exists, so the catalog is built from it here rather than
  // spelled out a second time in the App.
  void registerSettings();

  // Park for an OTA: drop to the calm clock and hand back the effect pool, which
  // the web server needs to stream the image. Sticky until the next boot.
  void park();

  void onEnter(lgfx::LGFX_Device&) override;
  void onExit() override;
  void tick(lgfx::LGFX_Device&) override;
  // Tap belongs to the running effect (kick the ball, poke the fire, reverse
  // the tunnel); long press advances the roster and persists the pick, so the
  // panel and the settings pages can never disagree about what is on screen.
  void onTap() override;
  void onLongPress() override;
};

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
