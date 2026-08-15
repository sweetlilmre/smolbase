# Building your app

`src/app/` is yours. `src/core/` is plumbing — read it, call it, don't edit it.
Everything your code needs is reached through the types in `src/core/App.h`;
the vocabulary (App, Screen, Extension Surface, …) is defined in
[../CONTEXT.md](../CONTEXT.md).

The template ships with a complete worked example that touches every hook: the
**demo clock**, the device's identity (IP, hostname, NTP time) over a rotation
of full-screen 256-color demoscene effects, animated through the framebuffer at
30 FPS and switched by a long press. One file per class so you can gut it
piecewise: `src/app/DemoScreen.h/.cpp` (the animated screen — overlay,
timestep, effect roster), `src/app/effects/` (one class per effect behind the
`Effect` interface), `src/app/StockScreen.h/.cpp` (the direct-draw fallback
when the framebuffer is compiled out), `src/app/StockApp.cpp` (the App glue and
the `makeApp()` seam), `src/app/hex_color.h` (the shared `#RRGGBB` parser).
This guide walks the code at the end.

## The threading rule (read this first)

Every hook the template calls on you — `setup()`, `loop()`, `Screen::tick()`,
touch handlers, `onSystemEvent()` — runs on **core 1, from the main loop**.
Events raised on core 0 (WiFi callbacks, SNTP, HTTP) are queued and drained by
the main loop before they reach you. Your code is single-threaded by
construction: plain variables, no mutexes — unless you deliberately spawn a
FreeRTOS task.

**The one documented exception**: HTTP route handlers you register via
`App::registerRoutes()` run on the httpd task (**core 0**). Anything a handler
shares with the rest of your app needs protection, and a handler must never
touch the Display or Screens — post a `SysEvent` (or set an atomic flag your
`loop()` checks) instead. Rationale and consequences:
[ADR 0001](adr/0001-consumer-code-single-threaded-core1.md).

## The seam: `makeApp()`

The core finds your app through one link-time factory. Define it in `src/app/`
— forgetting it is a linker error, by design:

```cpp
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
```

Minimal app:

```cpp
#include "../core/App.h"

class MyApp : public App { /* override what you need */ };

App& makeApp() {
  static MyApp app;
  return app;
}
```

### More than one app in the repo

The repo carries two apps — the demo clock in `src/app/` and the weather
clock in `src/app-weather/` — and one PlatformIO env per app selects which
one links. Each env's `build_src_filter` excludes every *other* `src/app*/`
directory, so exactly one directory provides `makeApp()` per build (two
selected = duplicate-symbol linker error, zero = missing-symbol; both by
design). To add your own app, copy the pattern in `platformio.ini`:

```ini
[env:myapp]
build_src_filter = +<*> -<app/> -<app-weather/>
```

put your code in `src/app-myapp/`, and build with `pio run -e myapp`. A bare
`pio run` (and CI) builds **every** env, so no app in the repo can silently
rot. `src/core/` and `src/main.cpp` always compile; only the app directory
swaps.

## App lifecycle

- **`setup()`** — called once at boot, after every core module (config,
  display, touch, clock, network, web) is up. Register settings here, create
  your Screens, and claim the display with `Display::setActive(&myScreen)`.
- **`loop()`** — called every main-loop pass on core 1. The pass has a **soft
  latency budget of ~25 ms** (`SMOLBASE_LOOP_BUDGET_MS`): overrun it and touch
  responsiveness and event latency degrade. Debug builds log overruns to
  serial, so a slow pass tells on itself. Heavy work (TLS fetches, JSON
  parsing of large payloads) belongs in a FreeRTOS task you spawn — then you
  own the synchronization.

## Screens

A `Screen` is the unit of display ownership (`src/core/App.h`):

```cpp
class Screen {
public:
  virtual ~Screen() = default;
  virtual void onEnter(lgfx::LGFX_Device&) {}
  virtual void onExit() {}
  virtual void tick(lgfx::LGFX_Device&) {}
  virtual void onTap() {}
  virtual void onLongPress() {}
};
```

The contract:

- **`onEnter`** — paint the full screen once. The system never repaints
  between ticks; whatever you leave on the panel stays there.
- **`tick`** — called every main-loop pass. Draw **only when something
  changed** (dirty-flag pattern); an unconditional redraw burns your loop
  budget for nothing. An **animated** screen is the sanctioned exception:
  it redraws on a fixed timestep instead of a dirty flag — see
  "Animating through the framebuffer" below for the contract and the real
  costs.
- **`onTap` / `onLongPress`** — the capacitive pad's events, delivered to the
  active screen only. Default no-ops.

One screen is active at a time, via a slot — `Display::setActive(&screen)` —
no stack. The system takes the slot over during AP mode (to show provisioning
instructions) and restores your screen when the network comes up; your
`onEnter` runs again, so a full repaint from `onEnter` is not optional.

## Touch

You don't read the pad yourself. `src/core/Touch.*` boot-calibrates the
threshold, debounces edges, and classifies: held ≥ 600 ms fires
`onLongPress()` once while held; shorter is `onTap()` on release. Tunables
(`SMOLBASE_TOUCH_*`) are in `include/smolbase_config.h`.

## System events

`onSystemEvent(SysEvent)` delivers lifecycle notifications, on core 1, in
order (`src/core/Events.h`):

| Event | Meaning | What you typically do |
| --- | --- | --- |
| `NetworkUp` | STA connected, IP acquired | start fetching, mark screens dirty |
| `NetworkDown` | STA lost; auto-reconnect already running | show an offline hint; do not reconfigure WiFi |
| `ApModeEntered` | provisioning AP is up; the system owns the display | pause anything network-bound |
| `TimeSynced` | SNTP delivered real time; `time(nullptr)` is now meaningful | repaint clocks |
| `SettingsChanged` | the store was saved (settings UI or your own `save()`) | re-read your settings, mark dirty |
| `OtaStarting` | **flash write imminent** | see below |

**`OtaStarting` obligations**: an update is streaming into flash on core 0.
From this event on, stop drawing and stop allocating — flash writes stall the
other core's cache access, and heap churn risks fragmenting what the TLS/OTA
path needs. Park your app: set a flag that short-circuits `loop()` and
`tick()`, and wait for the reboot. There is no "OTA finished" event; success
ends in a restart.

## Settings: registering your own

Register settings in `setup()` (boot-time only, before the web server starts
serving) via `src/core/ConfigStore.h`:

```cpp
ConfigStore::registerString(SettingSection::App, "city",  "Weather city", "London");
ConfigStore::registerInt   (SettingSection::App, "poll",  "Poll interval (min)", 15, 1, 120);
ConfigStore::registerBool  (SettingSection::App, "metric", "Metric units", true);
ConfigStore::registerColor (SettingSection::App, "accent", "Accent color", "#00d0ff");

// Pick-from-a-catalog settings: the user chooses by LABEL, your code reads the
// VALUE, and both persist (value under "mode", label under the derived
// "mode_name"). Inline catalogs are validated on save; a catalog too big for
// flash goes in a served asset instead (registerChoiceUrl — that's how the
// system timezone works, over /zones.json) and the browser does the lookup.
static const SettingChoice kModes[] = {{"Compact", "c"}, {"Detailed", "d"}};
ConfigStore::registerChoice(SettingSection::App, "mode", "Display mode",
                            "Compact", "c", kModes, 2);
```

**The promise: registering = API + persistence + stock UI.** Registered
`App`-section settings are **auto-rendered** as an "App" tab in the served
settings page (`html/settings.html` renders `GET /api/settings`), saves flow
through `POST /api/settings` into the store, and you get
`SysEvent::SettingsChanged` when anything persists. Read values anywhere on
core 1:

```cpp
String city = ConfigStore::getString("city"); // falls back to the registered default
int32_t poll = ConfigStore::getInt("poll");   // ints are clamped to [min,max] on write
String mode = ConfigStore::getString("mode"); // a choice reads as its machine VALUE
```

Keys and labels must be string literals (the registry stores pointers), keys
are flat (no nesting), and the registry holds `SMOLBASE_MAX_SETTINGS` (24)
entries — system entries included.

Two knobs shape how the App tab presents itself, both boot-time calls from
`setup()`:

```cpp
ConfigStore::setAppNote("Tuning for the frobnicator.");  // blurb atop the App tab
ConfigStore::suppressAppTab();  // my custom UI owns the UX — no App tab at all
```

The suppress flag affects **stock rendering only**: the settings stay
registered, persisted, and served over `GET/POST /api/settings` — which is
exactly what your custom skin consumes (both travel the same contract, as
`appNote` / `appTabSuppressed`). So the ladder is:

- **register** — API + persistence + a free stock UI; one line per setting,
  zero HTML.
- **register + `suppressAppTab()`** — API + persistence; your own page owns
  the UX (see `html/index.html` for a worked custom skin).
- **don't register** — persistence only, no schema treatment at all: raw
  `getString/setString/…` plus `ConfigStore::save()` work on unregistered
  keys; `save()` is atomic (temp file + rename) and posts `SettingsChanged`.
  Right for machine-facing state no UI should ever touch.

## Secrets: API keys, tokens, webhook URLs

Settings are the wrong home for credentials: `settings.json` is world-readable
over the settings API, ships inside filesystem images, and dies with every
fs-OTA. The **secret store** (`src/core/Secrets.h`) is a separate NVS
namespace with none of those properties:

```cpp
#include "../core/Secrets.h"

Secrets::set("api_key", value);      // persist (survives fs-OTA)
String k = Secrets::get("api_key");  // "" when absent
if (!Secrets::has("api_key")) { /* prompt the user */ }
Secrets::clear("api_key");
```

Secrets are deliberately **not** settings: no registration, no labels, no
defaults, and nothing about them auto-renders or serializes. The web surface
is write-only by construction — `GET /api/secrets` returns an existence map
(`{"api_key": true}`), never values; `POST /api/secrets` stores a flat object
where `null` deletes a key. Let end users enter a secret from your own page:

```js
// store (or update) — the value is never echoed back by any endpoint
await fetch("/api/secrets", { method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ api_key: input.value }) });

// show "configured / not set" without ever fetching the value
const present = await fetch("/api/secrets").then(r => r.json());
badge.textContent = present.api_key ? "configured" : "not set";
```

**Be honest about the threat model**: this is plain NVS. It protects against
*accidental exposure* — the settings API, filesystem images, backups of
`settings.json` — not against an attacker holding the device, who can read
flash directly. Real at-rest encryption requires Espressif's flash + NVS
encryption (an irreversible eFuse burn) and is outside what this template
supports. Size limits are NVS's own: ~4000 bytes per value, in a ~20 KB
partition shared with the WiFi credentials.

Factory reset erases the **entire** NVS partition — WiFi credentials,
secrets, and any NVS data you stored yourself — plus `settings.json`. RF
calibration data regenerates on the following boot.

## HTTP routes

Override `registerRoutes(PsychicHttpServer&)` to add endpoints:

```cpp
void registerRoutes(PsychicHttpServer& server) override {
  server.on("/api/weather", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    return res->send(200, "application/json", "{\"temp\":21}");
  });
}
```

What the template guarantees (see `src/core/Web.h` — the order is structural,
PsychicHttp matches first-registered-first):

1. System API routes (`/api/status`, `/api/wifi*`, `/api/settings`,
   `/api/factory-reset`) register first — you cannot shadow them, and you may
   not claim `/api/*` system paths.
2. OTA (`/api/update`) registers second.
3. **Your routes register third** — before static assets, so a route named
   like a file wins.
4. Static assets (gzip-packed `html/`) and the AP-mode captive catch-all come
   last.

And the warning worth repeating: **handlers run on the httpd task, core 0**
(ADR 0001). Keep them small, never touch Display/Screens from them, and guard
anything shared with your core-1 code. `ConfigStore` getters/setters are
mutex-guarded and safe from handlers.

Static pages need no routes at all: drop files into `html/`, rebuild the
filesystem image (`pio run -t buildfs`), and they are packed as gzip and
served from LittleFS.

## Framebuffer modes

Pick a mode in `include/smolbase_config.h` (or `-D SMOLBASE_FRAMEBUFFER=...`):

| Mode | Cost | When |
| --- | --- | --- |
| `SMOLBASE_FB_NONE` | zero RAM | direct drawing, or an app-owned scratch sprite (see ADR 0004) |
| `SMOLBASE_FB_PALETTE_8` (default) | 57.6 KB static (.bss) | full-frame composition, 256 colors, palette animation |
| `SMOLBASE_FB_RGB332` | 57.6 KB static (.bss) | full-frame composition in true 8-bpp color |

Choose by what you draw: `PALETTE_8` treats color arguments as **raw palette
indices**, which is what enables palette-cycling animation — but every
*computed* color write is garbage in that mode: anti-aliased font blends and
`pushImage` color conversion produce RGB values, not indices. If your screen
uses AA fonts or true-color images, use `RGB332` (all draws convert
correctly, quantized to 3/3/2 bits) — or skip the core buffer entirely, as
the weather app does (next paragraph).

There is deliberately no full-frame RGB565 mode: at 115.2 KB it cannot be
statically allocated on this chip (it overflows DRAM), and heap-allocating it
eats most of the WiFi-era headroom on a no-PSRAM board. If you need full-color
composition, create a partial-frame 16-bpp `lgfx::LGFX_Sprite` of your own —
sized to the region you actually redraw — and push it where needed. The
weather app is the worked example of that pattern: a static 240×64 RGB565
band scratch, band painters composing at band-relative y and pushing clipped
to their rows — see [ADR 0004](adr/0004-weather-band-scratch-rendering.md)
for the full decision record, including why full-frame composition failed for
it (anti-aliased typography is expensive to re-rasterize and the ~24 ms push
is most of a 30 Hz budget).

In both 8-bpp modes you get an opt-in composition sprite: draw into
`Display::frame()`, then `Display::present()` pushes it to the panel in one
DMA-backed transfer; in `RGB332` mode there is also `present(y, h)` to push
one full-width band (~2 ms for 20 rows vs ~24 ms full-frame). It is a
composition tool, never a requirement — screens keep drawing direct via
`tick(LGFX_Device&)` if they prefer. (The shipped worked example uses the
framebuffer; built with `SMOLBASE_FB_NONE` it falls back to a direct-draw
identity screen — both paths compile.)

In `PALETTE_8` mode the default palette maps index *i* as **RGB332** (bits
`RRRGGGBB`), so `lgfx::color332(r, g, b)` yields a sensible index for any
color, `0x00` is black and `0xFF` is white. Customize entries with
`Display::frame().setPaletteColor(index, r, g, b)`; palette changes take
effect on the next `present()`. One trap: `TFT_*` color constants are
`uint16_t` and resolve as `color & 0xFF` on a palette sprite — always draw
into `frame()` with raw indices or `color332()`.

## Animating through the framebuffer

The Boing clock demonstrates the animation path end to end; these are the
facts it was built on (measured on hardware, ticket #46 on the Boing map):

| Cost | Measured |
| --- | --- |
| `present()` (full 240×240 push over 40 MHz SPI) | **~24 ms, blocking core 1** — this IS the frame-rate ceiling |
| Full representative frame render into `frame()` | ~2 ms |
| Sustained FPS, web server live and being polled | 35 free-running; **30 at the fixed timestep** |

What that means for your animated screen:

- **Fix your timestep.** Render only when ≥33 ms has elapsed since the last
  frame (30 Hz). Physics per-frame becomes deterministic (no `dt` plumbing),
  and the skipped passes between frames keep touch and events responsive.
  Free-running buys ~5 more FPS and costs both of those.
- **The loop budget reads differently for animated apps.** A frame costs
  ~26–28 ms of the pass, dominated by the blocking `present()` — an animated
  tick consumes the ~25 ms soft budget *by design*. The budget's spirit (don't
  starve touch sampling and the event queue) is preserved by the timestep: the
  passes between frames take microseconds.
- **Partial redraws: it depends on your compose cost.** If your frame renders
  in ~2 ms like the Boing scene (pre-rendered sprites, fills, lines), don't
  bother — dirty-rect bookkeeping saves under half a millisecond against the
  24 ms push; clear, redraw everything, present. But if your frame is
  anti-aliased typography (15–25 ms to re-rasterize), full-frame composition
  cannot hold 30 Hz at all: compose per-band on change and push only those
  rows (`present(y, h)` in RGB332 mode, or an app-owned scratch). The weather
  app learned this on-device — ADR 0004 records the arc.
- **Palette animation is free.** `setPaletteColor()` is a 3-byte write applied
  at the next `present()` — the Boing ball's whole rotation is 14 of them per
  frame (the authentic 1984 technique: the ball never rotates as pixels; see
  `docs/research/boing-ball-technique.md`).
- **Reserve palette indices deliberately.** The demo clock splits the palette:
  `0x00–0x7F` belongs to whichever effect is running, `0xF0–0xF5` to the
  overlay (black plus the five user-picked text colors), and the rest keeps its
  RGB332 identity. It restores every index it touched in `onExit()`, since
  palette edits are global to the shared `frame()`. Three things fall out of
  that split worth stealing: a power-of-two bank makes palette cycling a single
  masked add per pixel (`(value + phase) & 0x7F`); giving your text its own
  reserved entries means it renders in *exact* RGB instead of quantizing
  through `color332()`; and putting the overlay's entries at the *top* leaves
  the low range free for an effect that needs an unusual amount of it — the
  wormhole replays an original VGA palette of 225 colours starting at index 1.
- **Blitting pre-rendered art**: an 8-bpp sprite pushed into `frame()` with a
  transparent index copies raw palette indices (no color conversion) at
  ~1–1.5 ms for a 120-px ball. Author sprite pixels in the frame's index
  space.
- **Park on `OtaStarting`**, same as everyone — and if you moved your art to
  flash (`.rodata`), parking during OTA stops being politeness and becomes
  mandatory: reading flash-resident data while OTA writes flash can panic the
  chip (see issue #53 for the full trade-off).
- **Your memory ceiling is set by OTA, not by your app.** An update streams a
  ~1.2 MB image through the web server, and that needs room. The demo clock
  learned this the hard way: holding a 61 KB effect pool left ~59 KB free,
  which ran fine for days and then refused every firmware upload outright —
  on a device with no serial port, that is a hole with no ladder. If your app
  holds a large allocation, free it in your `OtaStarting` handler (the demo
  screen's `park()` drops to a static screen and releases the pool), and treat
  "can this device still be flashed?" as a requirement your allocation has to
  satisfy.

## The worked example: the demo clock

`src/app/DemoScreen.cpp` wires all of the above into one screen (with
`src/app/StockApp.cpp` as the App glue): the identity text drop-shadowed over a
rotation of full-screen 256-color effects — the Boing ball, plasma, fire, a
a wormhole, a rotozoomer, and a calm black clock — running at a fixed 30 Hz
through `frame()`/`present()`. Technique notes and sources for each effect:
[research/palette-effects.md](research/palette-effects.md). The parts worth
stealing:

**The screen owns the frame; the effects own the pixels.** `src/app/effects/`
holds one class per effect behind a two-method interface (`enter()` claims the
palette bank and builds whatever tables it needs, `step()` paints one frame).
The screen draws the clock on top afterwards, so no effect knows the time
exists. Adding a sixth is one file and one line in the roster table.

**One buffer, six effects.** They share a single byte pool (`fx::scratch()`),
sized to whichever effect is running through `Effect::scratchBytes()` and freed
when nothing needs it — the pre-rendered ball, the fire's heat map and the
rotozoomer's texture are never alive at once, and two of the effects declare
zero and hold nothing at all. A static buffer does not fit, and the linker says
so out loud: the DRAM segment already carries the 57.6 KB framebuffer. Size it
against what an **OTA** needs, not what your app can get away with — see the
animation notes above.

**The rotation is palette cycling, not pixels.** The ball is pre-rendered into
palette indices using the original demo's facet formula (`((lat&1)*7 + lon) %
14` over 8 latitude bands × 56 longitude facets). Each frame, 14
`setPaletteColor()` writes shift the red/white assignment by `SPIN_STEPS`
stripes — the ball appears to spin while not a single ball pixel is redrawn.
The wormhole takes it further and computes *nothing*: a pre-rendered field of
indices in flash, a window spiralling across it, and two palette rotations
running against each other. Its frame is 240 row copies and 225 palette writes,
which makes the most faithful effect in the roster also the cheapest.

**The animated tick is a fixed timestep, not a dirty flag:**

```cpp
void DemoScreen::tick(lgfx::LGFX_Device&) {
  auto& f = Display::frame();
  if (entered && ROSTER[idx].fx) {
    if (now - lastFrameMs < FRAME_MS) return; // 30 Hz gate; passes between
    lastFrameMs += FRAME_MS;                  // frames cost microseconds
    if (dirty) { dirty = false; applySettings(f); } // may switch effects
  } else {
    // The calm clock: classic dirty-draw — repaint only on settings change,
    // minute rollover, or the colon heartbeat.
    if (!dirty && !calmDue()) return;
    if (dirty) { dirty = false; applySettings(f); }
  }
  if (Effect* e = ROSTER[idx].fx) e->step(f); // ~2-3 ms, full frame
  else f.fillScreen(fx::UI_BLACK);
  drawIdentity(f);    // two text passes, ~2 ms
  Display::present(); // ~24 ms blocking push — the frame's real cost
}
```

Both disciplines live in one screen, chosen by what is running. The colon still
blinks at 1 Hz in either mode as visible proof the clock is live.

**Touch drives app state** (#60): `onTap()` goes to the running effect, which
decides what a tap means to it (kick the ball, poke the fire, reverse the
wormhole, reshape the plasma). `onLongPress()` belongs to the screen: it
advances the roster and persists the pick through `ConfigStore::setString` +
`save()`, riding the same `SettingsChanged` path a web save takes — the switch
itself happens in `applySettings()`, on the repaint, whichever surface asked
for it. Screen, settings page and landing page therefore cannot disagree about
what is on the panel.

The app glues the screen to the system — and registers its settings:

```cpp
void setup() override {
  ConfigStore::setAppNote("These render here for free — ..."); // blurb atop the App tab
  ConfigStore::registerColor(SettingSection::App, "col_hour", "Clock hour color", "#ffffff");
  // ... col_min, col_colon, col_host, col_ip likewise ...
  screen.registerSettings();    // the "effect" choice, built from the roster
  Display::setActive(&screen);
}

void onSystemEvent(SysEvent e) override {
  if (e == SysEvent::NetworkUp || e == SysEvent::TimeSynced ||
      e == SysEvent::SettingsChanged) screen.markDirty();
}
```

Those registrations are the whole app/config/html triangle in action.
**App**: the screen re-reads the colors and the chosen effect on every dirty
repaint, so a change lands on the panel live via `SettingsChanged`.
**Config**: the values persist in `settings.json` and ride the standard
`GET`/`POST /api/settings` contract — the settings UI's App tab renders all
six automatically, colors as pickers, the effect as a dropdown. **HTML**:
`html/index.html` renders the same six a second time as a custom skin — a
color picker for every app-section color setting, a Behaviour row for every
app-section bool or inline choice, driven by the schema's `type` (never by
sniffing values); register a seventh setting and it appears in both places
with zero HTML changes. That duplication is deliberate: the App tab is
the free UI registration buys (its `setAppNote()` blurb says so on the page),
index.html is the hand-built one, and both read and write the identical
contract — your app keeps whichever suits it, or suppresses the tab and keeps
only its own. Colors are stored as the `#RRGGBB` string an
`<input type="color">` speaks; the app parses hex once per repaint (`hexRgb`),
writes it straight into its reserved palette entries, and draws every string
twice — black offset +2,+2, then the real color — so the text stays legible
over whatever the effect is doing. The effect is a **Choice** setting, so it
persists as the flat `effect` + `effect_name` pair (ADR 0002) and the long
press writes both halves from the roster, which is the authoritative catalog.

`NetworkUp` and `TimeSynced` don't draw anything — they mark dirty and let the
next `tick` repaint on core 1, which is exactly the pattern your HTTP handlers
should copy (via `Events::post` or a flag) since *they* are not on core 1.

Useful core helpers for your screens: `Net::deviceName()`, `Net::ip()`,
`Net::isUp()`, `Net::rssi()` (`src/core/Net.h`); `Clock::isSynced()`,
`Clock::nowLocal(tm&)` (`src/core/Clock.h`); `Display::setBrightness(0-255)`
(`src/core/Display.h`).

## Identity polish

- Set your firmware version: `build_flags = -D SMOLBASE_FW_VERSION=\"1.0.0\"`
  in `platformio.ini` — it shows in `GET /api/status` and the settings page.
- Rename the device family: `SMOLBASE_NAME_PREFIX` in
  `include/smolbase_config.h` changes the default hostname/AP SSID
  (`smolbase-XXXX` → `yourthing-XXXX`).
