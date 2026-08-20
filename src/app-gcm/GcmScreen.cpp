// Band rendering (ADR 0004): a static RGB565 scratch sprite is composed at
// band-relative y, then pushed to the panel with a clip rect — no fillScreen,
// no flicker. Each band redraws independently; only the timestamp band redraws
// every second. Everything else redraws on data change only.
#include "GcmScreen.h"
#include "../core/Net.h"
#include "smolbase_config.h"
#include <cstdio>

namespace {

constexpr int W = 240;

// Band layout (absolute panel y, height). Derived from measured font heights:
// Font4 = 26 px, Font6 = 48 px.
constexpr int NAME_Y  = 4,   NAME_H  = 30;  // Font4 26 px + 4 px slack
constexpr int VAL_Y   = 38,  VAL_H   = 54;  // Font6 48 px + 6 px slack
constexpr int ARROW_Y = 96,  ARROW_H = 44;  // 36 px icon + 4 px pad each side
constexpr int SPARK_Y = 142, SPARK_H = 72;  // sparkline
constexpr int TS_Y    = 218, TS_H    = 20;  // updated-ago counter

constexpr int SCRATCH_H = SPARK_H;          // tallest band; 240×72×2 = 34.6 KB

constexpr uint32_t OVERLAY_MS = 5000; // identity overlay display duration

// RGB888 color constants — LovyanGFX resolves uint32_t as RGB888.
constexpr uint32_t C_BLACK = 0x000000;
constexpr uint32_t C_WHITE = 0xffffff;
constexpr uint32_t C_GREEN = 0x00cc44;
constexpr uint32_t C_RED   = 0xee3322;
constexpr uint32_t C_GRAY  = 0x888888;
constexpr uint32_t C_DGRAY = 0x444444;

uint8_t            scratchData[W * SCRATCH_H * 2];
lgfx::LGFX_Sprite  scratch;

void pushBand(lgfx::LovyanGFX& gfx, int y, int h) {
    gfx.setClipRect(0, y, W, h);
    scratch.pushSprite(&gfx, 0, y);
    gfx.clearClipRect();
}

uint32_t trendColor(const GcmData& d) {
    if (d.inRange == 1) return C_GREEN;
    if (d.inRange == 0) return C_RED;
    return C_WHITE;
}

// Multiply each RGB channel by pct/100 — used for soft anti-alias edges.
uint32_t dimColor(uint32_t c, uint8_t pct) {
    uint8_t r = uint8_t(((c >> 16) & 0xff) * pct / 100);
    uint8_t g = uint8_t(((c >>  8) & 0xff) * pct / 100);
    uint8_t b = uint8_t(((c      ) & 0xff) * pct / 100);
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

// 5-state trend arrow in an aw×ah box at scratch-relative (ax, ay).
//
// Singles (2,4): wide arrowhead (60% h) + centred shaft (40% h).
// Doubles (1,5): two equal chevrons stacked, no shaft — visually distinct from singles.
// Stable  (3):   horizontal shaft + right-pointing head.
void drawTrendArrow(int ax, int ay, int aw, int ah,
                    uint8_t arrow, uint32_t c) {
    int cx  = ax + aw / 2;
    int hh  = ah * 6 / 10;   // arrowhead height (60%)
    int sh  = ah - hh;        // shaft height (40%)
    int sw  = aw / 3;         // shaft width
    int sx  = cx - sw / 2;    // shaft left edge
    int mid = ay + ah / 2;

    switch (arrow) {
        case 5: // ↑↑ rapidly rising: two upward chevrons
            scratch.fillTriangle(cx,      ay,      ax, mid,      ax + aw, mid,      c);
            scratch.fillTriangle(cx,      mid,     ax, ay + ah,  ax + aw, ay + ah,  c);
            break;
        case 4: // ↑ rising: arrowhead + shaft
            scratch.fillTriangle(cx, ay, ax, ay + hh, ax + aw, ay + hh, c);
            scratch.fillRect(sx, ay + hh, sw, sh, c);
            break;
        case 3: { // → stable: horizontal shaft + right-pointing head
            int slen = aw * 55 / 100;
            int hw   = ah / 4;          // half-height of shaft
            scratch.fillRect(ax, mid - hw, slen, hw * 2, c);
            scratch.fillTriangle(ax + slen, ay, ax + aw, mid, ax + slen, ay + ah, c);
            break;
        }
        case 2: // ↓ falling: shaft + downward arrowhead
            scratch.fillRect(sx, ay, sw, sh, c);
            scratch.fillTriangle(ax, ay + sh, cx, ay + ah, ax + aw, ay + sh, c);
            break;
        case 1: // ↓↓ rapidly falling: two downward chevrons
            scratch.fillTriangle(ax, ay,   cx, mid,      ax + aw, ay,      c);
            scratch.fillTriangle(ax, mid,  cx, ay + ah,  ax + aw, mid,     c);
            break;
    }
}

void drawSparkline(const GcmData& d, int top, int bottom, uint32_t c) {
    if (d.sparkCount < 2) return;
    float mn = d.spark[0], mx = d.spark[0];
    for (uint8_t i = 1; i < d.sparkCount; i++) {
        if (d.spark[i] < mn) mn = d.spark[i];
        if (d.spark[i] > mx) mx = d.spark[i];
    }
    float span = mx - mn;
    if (span < 0.01f) span = 1;

    const int padL = 8, padR = 8;
    int plotW = W - padL - padR;
    int plotH = bottom - top;

    int px = 0, py = 0, lastX = 0, lastY = 0;
    for (uint8_t i = 0; i < d.sparkCount; i++) {
        int x = padL + (int)((long)plotW * i / (d.sparkCount - 1));
        int y = bottom - (int)((d.spark[i] - mn) / span * plotH);
        if (y < top)    y = top;
        if (y > bottom) y = bottom;
        if (i > 0) {
            // 3px solid core for thickness, dimmed ±2 fringe for soft AA edges.
            uint32_t cd = dimColor(c, 40);
            scratch.drawLine(px, py - 2, x, y - 2, cd);
            scratch.drawLine(px, py - 1, x, y - 1, c);
            scratch.drawLine(px, py,     x, y,     c);
            scratch.drawLine(px, py + 1, x, y + 1, c);
            scratch.drawLine(px, py + 2, x, y + 2, cd);
        }
        px = x; py = y;
        lastX = x; lastY = y;
    }

    // White dot at the latest (rightmost) reading — stands out against any line colour.
    scratch.fillCircle(lastX, lastY, 4, C_WHITE);
}

} // namespace

void GcmScreen::begin() {
    scratch.setColorDepth(16);
    scratch.setBuffer(scratchData, W, SCRATCH_H, 16);
}

void GcmScreen::onEnter(lgfx::LGFX_Device& gfx) {
    gfx.fillScreen(C_BLACK);
    dirty_ = true;
    overlayUntilMs_ = 0;
    overlayDirty_   = false;
}

void GcmScreen::onLongPress() {
    overlayUntilMs_ = millis() + OVERLAY_MS;
    overlayDirty_   = true;
}

void GcmScreen::tick(lgfx::LGFX_Device& gfx) {
    uint32_t now = millis();

    // Overlay: show on request; on expiry, force a full normal repaint.
    if (overlayDirty_) {
        overlayDirty_ = false;
        drawOverlay(gfx);
        return;
    }
    if (overlayUntilMs_ && now >= overlayUntilMs_) {
        overlayUntilMs_ = 0;
        gfx.fillScreen(C_BLACK); // clear overlay color from inter-band gaps
        dirty_ = true;
    }
    if (overlayUntilMs_) return; // still showing — nothing else to update

    if (dirty_) {
        dirty_ = false;
        drawName(gfx);
        if (!data.valid) {
            drawNotReady(gfx);
            return;
        }
        drawValue(gfx);
        drawArrow(gfx);
        drawSpark(gfx);
        drawTimestamp(gfx);
        lastTickSec_ = now / 1000;
    } else if (data.valid && data.lastOkMs) {
        uint32_t nowSec = now / 1000;
        if (nowSec != lastTickSec_) {
            lastTickSec_ = nowSec;
            drawTimestamp(gfx);
        }
    }
}

// ---- band painters ---------------------------------------------------------

void GcmScreen::drawName(lgfx::LovyanGFX& gfx) {
    scratch.fillRect(0, 0, W, NAME_H, C_BLACK);
    scratch.setFont(&lgfx::fonts::Font4);
    scratch.setTextSize(1);
    scratch.setTextDatum(lgfx::top_center);
    scratch.setTextColor(data.error ? C_RED : C_WHITE, C_BLACK);
    scratch.drawString(data.name[0] ? data.name : "CGM", W / 2, 0);
    pushBand(gfx, NAME_Y, NAME_H);
}

// Shown when data.valid is false — clears value/arrow/spark/timestamp bands
// and renders a centred message so the user knows why nothing is displayed.
void GcmScreen::drawNotReady(lgfx::LovyanGFX& gfx) {
    const char* msg = data.noCredentials ? "Enter credentials"
                    : data.loginError   ? "Login error!"
                    : data.error        ? "Fetch error!"
                    :                     "Connecting...";
    uint32_t    c   = data.error ? C_RED : C_GRAY;

    // Value band — centred message
    scratch.fillRect(0, 0, W, VAL_H, C_BLACK);
    scratch.setFont(&lgfx::fonts::Font4);
    scratch.setTextSize(1);
    scratch.setTextDatum(lgfx::middle_center);
    scratch.setTextColor(c, C_BLACK);
    scratch.drawString(msg, W / 2, VAL_H / 2);
    pushBand(gfx, VAL_Y, VAL_H);

    // Clear arrow + spark + timestamp
    scratch.fillRect(0, 0, W, ARROW_H, C_BLACK);
    pushBand(gfx, ARROW_Y, ARROW_H);
    scratch.fillRect(0, 0, W, SPARK_H, C_BLACK);
    pushBand(gfx, SPARK_Y, SPARK_H);
    scratch.fillRect(0, 0, W, TS_H, C_BLACK);
    pushBand(gfx, TS_Y, TS_H);
}

void GcmScreen::drawValue(lgfx::LovyanGFX& gfx) {
    uint32_t c = trendColor(data);
    scratch.fillRect(0, 0, W, VAL_H, C_BLACK);
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", data.glucose);
    scratch.setFont(&lgfx::fonts::Font6);
    scratch.setTextSize(1);
    scratch.setTextDatum(lgfx::top_center);
    scratch.setTextColor(c, C_BLACK);
    scratch.drawString(buf, W / 2, 0);
    pushBand(gfx, VAL_Y, VAL_H);
}

void GcmScreen::drawArrow(lgfx::LovyanGFX& gfx) {
    uint32_t c = trendColor(data);
    scratch.fillRect(0, 0, W, ARROW_H, C_BLACK);
    if (data.trendArrow >= 1 && data.trendArrow <= 5) {
        const int as = 36;
        drawTrendArrow((W - as) / 2, (ARROW_H - as) / 2, as, as, data.trendArrow, c);
    }
    pushBand(gfx, ARROW_Y, ARROW_H);
}

void GcmScreen::drawSpark(lgfx::LovyanGFX& gfx) {
    uint32_t c = trendColor(data);
    scratch.fillRect(0, 0, W, SPARK_H, C_BLACK);
    // Sparkline coordinates are scratch-relative: top=0, bottom=SPARK_H-2
    drawSparkline(data, 0, SPARK_H - 2, c);
    pushBand(gfx, SPARK_Y, SPARK_H);
}

void GcmScreen::drawTimestamp(lgfx::LovyanGFX& gfx) {
    if (!data.lastOkMs) return;
    uint32_t ago = (millis() - data.lastOkMs) / 1000;
    char buf[12];
    if (ago < 3600) snprintf(buf, sizeof(buf), "%lus", (unsigned long)ago);
    else            snprintf(buf, sizeof(buf), "%lum", (unsigned long)(ago / 60));
    scratch.fillRect(0, 0, W, TS_H, C_BLACK);
    scratch.setFont(&lgfx::fonts::Font2);
    scratch.setTextSize(1);
    scratch.setTextDatum(lgfx::top_left);
    scratch.setTextColor(data.error ? C_RED : C_DGRAY, C_BLACK);
    scratch.drawString(buf, 4, 2);
    pushBand(gfx, TS_Y, TS_H);
}

// Full-screen identity overlay: hostname, IP, firmware version — same info
// the weather app's long-press exposes (WeatherScreen::drawClockBand).
// Paints the entire panel directly so it doesn't depend on scratch height;
// expires after OVERLAY_MS and the normal GCM content repaints underneath.
void GcmScreen::drawOverlay(lgfx::LGFX_Device& gfx) {
    constexpr uint32_t C_OVL_BG = 0x001133;
    constexpr uint32_t C_CYAN   = 0x99ffff;
    gfx.fillScreen(C_OVL_BG);
    gfx.setFont(&lgfx::fonts::Font4);
    gfx.setTextSize(1);
    gfx.setTextDatum(lgfx::middle_center);
    gfx.setTextColor(C_WHITE, C_OVL_BG);
    gfx.drawString(Net::deviceName(), W / 2, 90);
    gfx.setTextColor(C_CYAN, C_OVL_BG);
    gfx.drawString(Net::ip().toString(), W / 2, 120);
    gfx.setTextColor(C_WHITE, C_OVL_BG);
    gfx.drawString(SMOLBASE_FW_VERSION, W / 2, 150);
}
