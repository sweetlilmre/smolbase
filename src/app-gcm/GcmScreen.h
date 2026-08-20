#pragma once
#include "../core/App.h"
#include "GcmData.h"
#include <LovyanGFX.hpp>

class GcmScreen : public Screen {
public:
    GcmData data;

    void begin();            // bind scratch buffer; call once from setup
    void markDirty() { dirty_ = true; }

    void onEnter(lgfx::LGFX_Device& gfx) override;
    void tick(lgfx::LGFX_Device& gfx) override;
    void onLongPress() override; // show device identity overlay for 5 s

private:
    bool     dirty_          = true;
    uint32_t lastTickSec_    = 0;
    uint32_t overlayUntilMs_ = 0; // non-zero while identity overlay is visible
    bool     overlayDirty_   = false;

    void drawName(lgfx::LovyanGFX& gfx);
    void drawNotReady(lgfx::LovyanGFX& gfx);
    void drawValue(lgfx::LovyanGFX& gfx);
    void drawArrow(lgfx::LovyanGFX& gfx);
    void drawSpark(lgfx::LovyanGFX& gfx);
    void drawTimestamp(lgfx::LovyanGFX& gfx);
    void drawOverlay(lgfx::LGFX_Device& gfx);
};
