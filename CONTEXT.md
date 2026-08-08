# Smolbase — Ubiquitous Language

## Glossary

- **Smolbase** — this project: a template firmware for the Small TV Pro, shipping just enough infrastructure (provisioning, web server, config, time, display, OTA) for Consumers to build their own firmware on top.
- **Consumer** — a developer who clones the template repo and builds their own firmware on it. Distinct from the **End User**, who owns a flashed device.
- **Provisioning** — getting an unconfigured device onto a WiFi network via the Captive Portal.
- **Captive Portal** — the WiFi-provisioning-only web page served while the device is in AP Mode. Its single job is scan-and-join; it never exposes other settings.
- **AP Mode** — the device acting as its own access point (`smolbase-XXXX`) because it has no joinable network. The screen shows connection instructions while in this mode.
- **AP Fallback** — automatically returning to AP Mode when the stored network cannot be reached, whether at boot or after sustained runtime loss.
- **Settings UI** — the configuration web page served once the device is on a network. Covers: timezone, NTP server, brightness, hostname, WiFi re-configure, OTA, factory reset.
- **Stock Screen** — the default on-network display (IP, hostname, NTP-synced time). It is the demo implementation of the App Screen extension point; Consumers replace it.
- **App Screen** — the extension point where a Consumer's own UI lives.
- **Config Store** — split persistence: WiFi credentials in NVS (the WiFi stack's native home); all other settings as JSON on LittleFS, extensible by Consumers.
- **Extension Surface** — the set of hooks Smolbase offers Consumers: the App Screen, HTTP route registration, settings schema extension, touch input events, lifecycle hooks. (Exact shape: open ticket.)
- **Factory Reset** — clearing credentials and settings via the Settings UI, returning the device to AP Mode.
