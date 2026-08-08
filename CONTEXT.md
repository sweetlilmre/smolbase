# Smolbase — Ubiquitous Language

## Glossary

- **Smolbase** — this project: a template firmware for the Small TV Pro, shipping just enough infrastructure (provisioning, web server, config, time, display, OTA) for Consumers to build their own firmware on top.
- **Consumer** — a developer who clones the template repo and builds their own firmware on it. Distinct from the **End User**, who owns a flashed device.
- **Provisioning** — getting an unconfigured device onto a WiFi network via the Captive Portal.
- **Captive Portal** — the WiFi-provisioning-only web page served while the device is in AP Mode. Its single job is scan-and-join; it never exposes other settings.
- **AP Mode** — the device acting as its own access point (`smolbase-XXXX`) because it has no joinable network. The screen shows connection instructions while in this mode.
- **AP Fallback** — entering AP Mode at boot when there are no stored credentials or the stored network cannot be joined within the connect timeout. Boot-time only: a runtime WiFi drop triggers silent auto-reconnect, forever, never AP Mode. (Amended from the charter, which allowed runtime fallback.)
- **Settings UI** — the configuration web page served once the device is on a network. Covers: timezone, NTP server, brightness, hostname, WiFi re-configure, OTA, factory reset.
- **Stock Screen** — the default on-network display (IP, hostname, NTP-synced time). It is the demo implementation of the App Screen extension point; Consumers replace it.
- **App Screen** — the extension point where a Consumer's own UI lives.
- **Config Store** — split persistence: WiFi credentials in NVS (the WiFi stack's native home); all other settings as JSON on LittleFS, extensible by Consumers.
- **Extension Surface** — the set of hooks Smolbase offers Consumers, all reached from `src/app/` (*yours*) while `src/core/` stays *plumbing*: the App, Screens, route registration, the Settings Schema, touch events, and System Events.
- **App** — the Consumer's code: a class the core reaches through a link-time `makeApp()` factory in `src/app/`, driven by `setup()` at boot and `loop()` every main-loop pass (core 1). Soft latency budget ~25 ms per pass (debug builds log overruns); heavy work belongs in a Consumer-spawned FreeRTOS task.
- **Screen** — the unit of display ownership: `onEnter` (paint once), `onExit`, `tick(display)` (update only when dirty — the system never repaints between ticks), plus default no-op touch handlers (tap / long-press). One active Screen at a time via a slot (`setActive`), no stack; the system overrides the slot during AP Mode and restores it after.
- **System Event** — lifecycle notification delivered to the App via `onSystemEvent(Event)`: network up/down, entering/leaving AP Mode, time synced, OTA starting.
- **Settings Schema** — settings the App registers in code (key, type, label, default, range); the served Settings UI auto-renders them in an "App" section, persistence flows through the web server into the Config Store, and the App is notified on change. Raw Config Store access remains underneath for what the schema can't express.
- **Factory Reset** — scorched-earth return to "device as shipped" via the Settings UI: full NVS erase (WiFi credentials, secrets, any consumer NVS data; RF calibration regenerates next boot) plus settings.json removal, landing in AP Mode.
- **Secret Store** — consumer-held opaque values (API keys, tokens, webhook URLs) in a dedicated NVS namespace, deliberately outside the Settings Schema: nothing auto-renders or serializes. Write-only over the web (`POST /api/secrets`; GET returns an existence map, never values). Survives fs-OTA; dies with Factory Reset. Plain NVS: protection against accidental exposure, not physical flash access.
- **Forget WiFi** — clearing only the stored credentials (settings survive) via the Settings UI's WiFi tab, returning the device to AP Mode for re-provisioning. The same tab can also scan-and-join a new network directly, restarting with settings intact.
