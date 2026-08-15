#include "DemoScreen.h"

#if SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8

#include "../core/Clock.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "effects/BoingEffect.h"
#include "effects/Effect.h"
#include "effects/FireEffect.h"
#include "effects/PlasmaEffect.h"
#include "effects/RotozoomEffect.h"
#include "effects/TunnelEffect.h"
#include "hex_color.h"
#include <Arduino.h>
#include <ctime>

namespace {

// ---- The roster --------------------------------------------------------------
// One table, three jobs: what long press cycles through, what the settings
// catalog offers, and which class paints. `fx == nullptr` is the calm clock —
// no effect at all, black behind the identity, and the only entry that runs on
// the dirty-draw discipline instead of the 30 Hz timestep.
BoingEffect boing;
PlasmaEffect plasma;
FireEffect fire;
TunnelEffect tunnel;
RotozoomEffect rotozoom;

struct RosterEntry {
  const char* label; // what the user sees, on the panel and in the settings UI
  const char* value; // what persists in settings.json
  Effect* fx;
};

const RosterEntry ROSTER[] = {
    {"Boing ball", "boing", &boing},   {"Plasma", "plasma", &plasma},
    {"Fire", "fire", &fire},           {"Tunnel", "tunnel", &tunnel},
    {"Rotozoomer", "roto", &rotozoom}, {"Calm clock", "calm", nullptr},
};
constexpr int ROSTER_N = (int)(sizeof(ROSTER) / sizeof(ROSTER[0]));
constexpr int CALM_IDX = ROSTER_N - 1; // the fx == nullptr entry, last by convention

// The catalog handed to registerChoice — the pointer must outlive registration,
// hence file scope; the contents are copied out of the roster so the two can
// never drift.
SettingChoice choices[ROSTER_N];

int rosterIndexOf(const String& value) {
  for (int i = 0; i < ROSTER_N; ++i)
    if (value == ROSTER[i].value) return i;
  return -1;
}

constexpr uint32_t FRAME_MS = 33;      // fixed 30 Hz timestep (ticket #47)
constexpr uint32_t NAME_MS = 1500;     // how long the "now showing" banner lingers
constexpr uint8_t IDX_WHITE = 0xFF;    // untouched RGB332 identity: 0xFF is white

} // namespace

void DemoScreen::registerSettings() {
  for (int i = 0; i < ROSTER_N; ++i) choices[i] = {ROSTER[i].label, ROSTER[i].value};
  ConfigStore::registerChoice(SettingSection::App, "effect", "Screen effect",
                              ROSTER[0].label, ROSTER[0].value, choices, ROSTER_N);
}

// Both palette halves are written here, and only here: the effect's own bank is
// rewritten by its enter() below, the identity colors land verbatim (no
// color332() quantization — the overlay owns real palette entries now).
void DemoScreen::applySettings(lgfx::LGFX_Sprite& f) {
  colHour = hexRgb(ConfigStore::getString("col_hour"), 0xffffff);
  colMin = hexRgb(ConfigStore::getString("col_min"), 0xffffff);
  colColon = hexRgb(ConfigStore::getString("col_colon"), 0xffffff);
  colHost = hexRgb(ConfigStore::getString("col_host"), 0xffffff);
  colIp = hexRgb(ConfigStore::getString("col_ip"), 0xffffff);
  const uint32_t ui[] = {colHour, colMin, colColon, colHost, colIp};
  f.setPaletteColor(fx::UI_BLACK, 0x00, 0x00, 0x00);
  for (int i = 0; i < 5; ++i)
    f.setPaletteColor((uint8_t)(fx::UI_TEXT + i), (uint8_t)(ui[i] >> 16),
                      (uint8_t)(ui[i] >> 8), (uint8_t)ui[i]);

  // The effect is settings-driven, both ways round: long press writes the key
  // and a web save writes the key, and either way the switch happens HERE, on
  // the SettingsChanged repaint. One path, so the three surfaces cannot drift.
  const int want = rosterIndexOf(ConfigStore::getString("effect"));
  if (want >= 0 && (want != idx || !entered)) select(want, f);
}

void DemoScreen::select(int i, lgfx::LGFX_Sprite& f) {
  // Every effect but the calm clock needs the shared scratch. Its one 14.4 KB
  // allocation cannot realistically fail at boot, but if it ever does the
  // screen degrades to the entry that needs nothing rather than to a crash.
  if (ROSTER[i].fx && !fx::scratchReady()) i = CALM_IDX;
  if (entered && i != idx) nameShownMs = millis(); // announce, but not at boot
  idx = i;
  entered = true;
  lastFrameMs = millis();
  lastMinute = -1; // force the calm clock to repaint if that is what we landed on
  if (ROSTER[idx].fx) ROSTER[idx].fx->enter(f);
}

bool DemoScreen::calmDue() const {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  const bool colonNow = !Clock::isSynced() || (t.tm_sec & 1) == 0;
  return t.tm_min != lastMinute || colonNow != colonOn;
}

void DemoScreen::shadowString(lgfx::LGFX_Sprite& f, const String& s, int x, int y,
                              uint8_t idx) {
  f.setTextColor(fx::UI_BLACK);
  f.drawString(s, x + 2, y + 2);
  f.setTextColor(idx);
  f.drawString(s, x, y);
}

// The identity overlay, every string drop-shadowed (ticket #51): drawn once in
// black offset down-right, then in its real color on top — on an indexed
// framebuffer that is just a second cheap text pass, and it keeps the text
// readable over whatever the effect is doing behind it.
// Hour and minute wear their own colors, so they are drawn as separate strings
// hung off the colon cell; the colon blinks at 1 Hz as visible proof the clock
// is live (solid until first NTP sync).
void DemoScreen::drawIdentity(lgfx::LGFX_Sprite& f) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  lastMinute = t.tm_min;
  colonOn = !Clock::isSynced() || (t.tm_sec & 1) == 0;
  char hh[3] = "--", mm[3] = "--";
  if (Clock::isSynced()) {
    snprintf(hh, sizeof(hh), "%02d", t.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  }
  f.setFont(&fonts::FreeSansBold24pt7b);
  int w = f.textWidth(":");
  f.setTextDatum(lgfx::middle_right);
  shadowString(f, hh, 120 - w / 2, 100, fx::UI_TEXT + 0);
  f.setTextDatum(lgfx::middle_left);
  shadowString(f, mm, 120 + w / 2, 100, fx::UI_TEXT + 1);
  if (colonOn) {
    f.setTextDatum(lgfx::middle_center);
    shadowString(f, ":", 120, 100, fx::UI_TEXT + 2);
  }
  f.setFont(&fonts::FreeSans9pt7b);
  f.setTextDatum(lgfx::middle_center);
  shadowString(f, Net::deviceName() + ".local", 120, 160, fx::UI_TEXT + 3);
  shadowString(f, Net::isUp() ? Net::ip().toString() : "connecting...", 120, 185,
               fx::UI_TEXT + 4);
  // "Now showing": one long press is the only way to discover the roster on a
  // device with a single touch pad, so it says what it just switched to.
  if (nameShownMs && millis() - nameShownMs < NAME_MS)
    shadowString(f, ROSTER[idx].label, 120, 216, IDX_WHITE);
}

void DemoScreen::onEnter(lgfx::LGFX_Device&) {
  dirty = true;
  entered = false; // applySettings() will enter whichever effect is stored
  lastFrameMs = 0;
}

void DemoScreen::onExit() {
  // Palette edits are global to the shared frame(): hand every index this
  // screen touched back as RGB332 identity, so the next owner gets the
  // documented contract.
  auto& f = Display::frame();
  for (int i = 0; i <= fx::UI_LAST; ++i)
    f.setPaletteColor(i, ((i >> 5) & 0x07) * 255 / 7, ((i >> 2) & 0x07) * 255 / 7,
                      (i & 0x03) * 255 / 3);
}

// Two disciplines in one screen, chosen by what is running: an effect renders
// on a fixed 30 Hz timestep (deterministic animation, and the skipped passes
// between frames keep touch and events responsive), the calm clock renders
// only when the displayed state changes.
void DemoScreen::tick(lgfx::LGFX_Device&) {
  auto& f = Display::frame();
  if (entered && ROSTER[idx].fx) {
    const uint32_t now = millis();
    if (now - lastFrameMs < FRAME_MS) return;
    lastFrameMs += FRAME_MS; // catch-up scheduling holds the average...
    if (now - lastFrameMs >= FRAME_MS) lastFrameMs = now; // ...a long stall resets it
    if (dirty) {
      dirty = false;
      applySettings(f); // may hand the panel to a different effect entirely
    }
  } else {
    if (!dirty && !calmDue()) return;
    if (dirty) {
      dirty = false;
      applySettings(f);
    }
  }

#ifdef SMOLBASE_DEBUG
  // Where the 33 ms goes, once a second. The claim this roster is built on is
  // that an effect fits in what present() leaves over — this is how to check it
  // on real hardware rather than trusting the arithmetic.
  const uint32_t t0 = micros();
  if (Effect* e = ROSTER[idx].fx) e->step(f);
  else f.fillScreen(fx::UI_BLACK);
  const uint32_t t1 = micros();
  drawIdentity(f);
  const uint32_t t2 = micros();
  Display::present();
  const uint32_t t3 = micros();
  static uint32_t lastLog = 0;
  if (t3 - lastLog > 1000000) {
    lastLog = t3;
    Serial.printf("[demo] %s: effect %lu us, overlay %lu us, present %lu us\n",
                  ROSTER[idx].label, t1 - t0, t2 - t1, t3 - t2);
  }
#else
  if (Effect* e = ROSTER[idx].fx) e->step(f);
  else f.fillScreen(fx::UI_BLACK);
  drawIdentity(f);
  Display::present(); // ~24 ms blocking push — the frame's real cost
#endif
}

void DemoScreen::onTap() {
  if (Effect* e = ROSTER[idx].fx) e->onTap();
  else markDirty(); // the calm clock's only trick: repaint on demand
}

// The physical twin of the settings UI's "Screen effect" dropdown: write the
// pick and save. save() posts SettingsChanged, which marks the screen dirty,
// and applySettings() does the actual switching — the exact path a web save
// takes. Both halves of the choice are written together (ADR 0002): the roster
// is the authoritative catalog, so the label is taken from it, not derived.
void DemoScreen::onLongPress() {
  const int next = (idx + 1) % ROSTER_N;
  ConfigStore::setString("effect", ROSTER[next].value);
  ConfigStore::setString("effect_name", ROSTER[next].label);
  ConfigStore::save();
}

#endif // SMOLBASE_FRAMEBUFFER == SMOLBASE_FB_PALETTE_8
