# Building your app

`src/app/` is yours. `src/core/` is plumbing — read it, call it, don't edit it.
Everything your code needs is reached through the types in `src/core/App.h`;
the vocabulary (App, Screen, Extension Surface, …) is defined in
[../CONTEXT.md](../CONTEXT.md).

The template ships with `src/app/StockApp.cpp` — a complete worked example that
touches every hook. Gut it and replace it; this guide walks its code at the end.

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
  budget for nothing.
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
```

That is the whole feature: registered `App`-section settings are
**auto-rendered** as an "App" tab in the served settings page
(`html/settings.html` renders `GET /api/settings`), saves flow through
`POST /api/settings` into the store, and you get `SysEvent::SettingsChanged`
when anything persists. Read values anywhere on core 1:

```cpp
String city = ConfigStore::getString("city"); // falls back to the registered default
int32_t poll = ConfigStore::getInt("poll");   // ints are clamped to [min,max] on write
```

Keys and labels must be string literals (the registry stores pointers), keys
are flat (no nesting), and the registry holds `SMOLBASE_MAX_SETTINGS` (24)
entries — system entries included. For anything the schema can't express, raw
`getString/setString/…` plus `ConfigStore::save()` work on unregistered keys
too; `save()` is atomic (temp file + rename) and posts `SettingsChanged`.

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
| `SMOLBASE_FB_NONE` | zero RAM | direct drawing only |
| `SMOLBASE_FB_PALETTE_8` (default) | 57.6 KB static (.bss) | full-frame composition, 256 colors |

There is deliberately no full-frame RGB565 mode: at 115.2 KB it cannot be
statically allocated on this chip (it overflows DRAM), and heap-allocating it
eats most of the WiFi-era headroom on a no-PSRAM board. If you need full-color
composition, create a partial-frame 16-bpp `lgfx::LGFX_Sprite` of your own —
sized to the region you actually redraw — and push it where needed.

In `PALETTE_8` mode you get an opt-in composition sprite: draw into
`Display::frame()`, then `Display::present()` pushes it to the panel in one
DMA-backed transfer. It is a composition tool, never a requirement — screens
keep drawing direct via `tick(LGFX_Device&)` if they prefer.

In `PALETTE_8` mode the default palette maps index *i* as **RGB332** (bits
`RRRGGGBB`), so `lgfx::color332(r, g, b)` yields a sensible index for any
color, `0x00` is black and `0xFF` is white. Customize entries with
`Display::frame().setPaletteColor(index, r, g, b)`; palette changes take
effect on the next `present()`.

## The worked example: StockApp

`src/app/StockApp.cpp` wires all of the above into ~100 lines. The screen owns
a dirty flag, the last-drawn minute, and the colon blink phase:

```cpp
class StockScreen : public Screen {
  bool dirty = true;
  int lastMinute = -1;
  bool colonOn = true;

public:
  void markDirty() { dirty = true; }

  void onEnter(lgfx::LGFX_Device& d) override {
    d.fillScreen(TFT_BLACK); // full paint: the system never repaints for you
    dirty = true;
    lastMinute = -1;
  }

  void tick(lgfx::LGFX_Device& d) override {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    bool colonNow = !Clock::isSynced() || (t.tm_sec & 1) == 0;
    if (!dirty && t.tm_min == lastMinute) {
      if (colonNow != colonOn) { colonOn = colonNow; drawColon(d); }
      return; // draw only on change
    }
    ...
  }

  void onTap() override { markDirty(); } // demo: any tap forces a repaint
};
```

Note the shape of `tick`: the early-out is the whole performance story. The
colon blinks at 1 Hz as visible proof the screen is live, and even that
heartbeat honors the discipline — `drawColon` overdraws only the colon's own
cell, never the digits. The full draw likewise fills only the rectangles it
rewrites (`fillRect` + `drawString`), never the whole screen.

The app glues the screen to the system — and registers its own settings:

```cpp
class StockApp : public App {
  StockScreen screen;

public:
  void setup() override {
    ConfigStore::registerString(SettingSection::App, "col_hour", "Clock hour color", "#ffffff");
    // ... col_min, col_host, col_ip likewise ...
    Display::setActive(&screen);
  }

  void onSystemEvent(SysEvent e) override {
    if (e == SysEvent::NetworkUp || e == SysEvent::TimeSynced ||
        e == SysEvent::SettingsChanged) screen.markDirty();
  }
};

App& makeApp() {
  static StockApp app;
  return app;
}
```

Those four registrations are the whole app/config/html triangle in action.
**App**: the screen re-reads the colors on every full repaint and reacts to
`SettingsChanged` by marking dirty, so a change lands on the panel live.
**Config**: the values persist in `settings.json` and ride the standard
`GET`/`POST /api/settings` contract — the settings UI's App section renders
them automatically. **HTML**: `html/index.html` goes one step further and
renders a color picker for every app-section string setting holding a
`#RRGGBB` value — register a fifth color and it appears there with zero HTML
changes. Colors are stored as the `#RRGGBB` string an `<input type="color">`
speaks; the app parses hex once per repaint (`hexRgb` in StockApp.cpp).

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
