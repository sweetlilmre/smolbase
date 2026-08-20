// Direct-draw identity screen — the app's whole display when the framebuffer
// is compiled out (SMOLBASE_FRAMEBUFFER=SMOLBASE_FB_NONE); see git history for
// its dirty-draw walkthrough.
#pragma once
#include "../core/App.h"

class StockScreen : public Screen {
  bool dirty = true;
  int lastMinute = -1;
  bool colonOn = true;
  uint32_t colHour = 0xffffff, colMin = 0xffffff, colColon = 0xffffff,
           colHost = 0xffffff, colIp = 0xffffff;

  void loadColors();
  void drawColon(lgfx::LGFX_Device& d);

public:
  void markDirty() { dirty = true; }

  void onEnter(lgfx::LGFX_Device& d) override;
  void tick(lgfx::LGFX_Device& d) override;
  void onTap() override { markDirty(); }
};
