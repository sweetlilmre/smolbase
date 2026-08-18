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

private:
    bool     dirty_       = true;
    uint32_t lastTickSec_ = 0;

    void drawName(lgfx::LovyanGFX& gfx);
    void drawValue(lgfx::LovyanGFX& gfx);
    void drawArrow(lgfx::LovyanGFX& gfx);
    void drawSpark(lgfx::LovyanGFX& gfx);
    void drawTimestamp(lgfx::LovyanGFX& gfx);
};
