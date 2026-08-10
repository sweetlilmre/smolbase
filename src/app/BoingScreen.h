// The Boing clock screen (PALETTE_8 framebuffer builds only): the Amiga Boing
// Ball (1984, Dale Luck & R.J. Mical) bouncing behind the device's identity
// (IP, hostname, time), driven through the frame()/present() animation path.
// Its signature trick: the ball never rotates as pixels, 14 cycling palette
// entries do the work. Technique details: docs/research/boing-ball-technique.md;
// palette budget, geometry, and tuning constants live in BoingScreen.cpp.
#pragma once
#include "../core/App.h"
#include "../core/Display.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

class BoingScreen : public Screen {
  bool dirty = true;
  bool enabled = true, paused = false, ballOk = false;
  int lastMinute = -1;
  bool colonOn = true;
  uint32_t lastFrameMs = 0;
  int cyclePhase = 0;
  float bx = 120, by = 90, vx = 2.2f, vy = 0;
  // The pre-rendered ball. Its 14.4 KB pixel buffer is the one deliberate heap
  // allocation in the app — a static buffer overflowed the DRAM data segment
  // (the 57.6 KB framebuffer lives there already). Allocated once at boot and
  // never freed, same footing as the framebuffer's own palette storage. Its
  // palette contents never matter: palette-8 → palette-8 pushes copy raw
  // indices, and the frame's palette decides the colors (research #45).
  lgfx::LGFX_Sprite ball;

  uint32_t colHour = 0xffffff, colMin = 0xffffff, colColon = 0xffffff,
           colHost = 0xffffff, colIp = 0xffffff;

  void loadSettings();
  void applyCycle(lgfx::LGFX_Sprite& f);
  void stepPhysics();
  void drawShadow(lgfx::LGFX_Sprite& f, int cx, int cy);
  static void shadowString(lgfx::LGFX_Sprite& f, const String& s, int x, int y,
                           uint8_t idx);
  void drawScene(lgfx::LGFX_Sprite& f);

public:
  void markDirty() { dirty = true; }
  void togglePause() { paused = !paused; }

  void onEnter(lgfx::LGFX_Device&) override;
  void onExit() override;
  void tick(lgfx::LGFX_Device&) override;
  // Tap pauses/resumes the bounce — touch driving app state. (When the ball is
  // off it just forces a repaint, the previous demo behavior.)
  void onTap() override;

  void begin(); // pre-render the ball (boot-time, once)
};

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
