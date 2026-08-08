---
title: Smolbase — MVP template firmware for the Small TV Pro
labels: [wayfinder:map]
---

## Destination

The MVP template firmware **smolbase** builds, flashes, and demonstrates the full lifecycle on a Small TV Pro: first-run AP mode + captive portal → joined to WiFi → stock screen with IP/hostname/NTP time → settings UI (timezone, NTP, brightness, hostname, WiFi, OTA, factory reset) → AP fallback when the network is lost. Clean, modular, memory-lean, ready for Consumers to build on.

## Notes

- **Execution override**: this map carries execution — the destination is a *built* MVP, not just a spec.
- Skills every session should consult: `/grilling` + `/domain-modeling` for decisions (glossary in `CONTEXT.md`), `/prototype` for UI tickets, `/research` for AFK research, `/tdd` where logic warrants it during build tickets.
- Reference project: `D:\source\SmolTV-Pro` — patterns and hardware facts only; **never copy its source**. Hardware facts and all baseline decisions live in [Charter grilling — destination and baseline decisions](tickets/0001-charter-grilling.md).
- Tracker conventions: [wayfinder/README.md](README.md) (local-markdown fallback).
- Standing preferences: speed- and memory-optimal; static allocation over heap; Python tooling via uv in a venv.

## Decisions so far

- [Charter grilling — destination and baseline decisions](tickets/0001-charter-grilling.md) — destination, scope (+OTA), PlatformIO+Arduino, PsychicHttp, template-repo model, split config store, POSIX TZ dropdown, AP-fallback reprovisioning, touch events, stock screen as extension point.

## Not yet specified

Everything here is in scope but not yet sharp enough to ticket; it graduates as the frontier advances (mostly once *Module architecture and boot/network state machine* and the research tickets close):

- **Build slices of the MVP** — after scaffolding: WiFi manager + AP fallback state machine; captive portal (DNS + portal page + join flow); config store implementation; settings UI backend + frontend; NTP + timezone application; display driver + AP-info screen + stock screen; touch driver + events; mDNS; OTA (firmware + filesystem). Slicing and ordering fall out of the architecture ticket.
- **Asset pack tooling** — gzip pack script details (uv project layout, LittleFS image build), sharpens with the scaffold.
- **Consumer documentation** — README, "build your app here" guide, flashing instructions for a board with no USB-serial bridge; sharpens after the extension surface is decided.
- **CI smoke build** — worth having for a template repo; shape depends on the scaffold.

## Out of scope

- **The reference firmware's apps** (clock/coin/weather/monitor/etc.) and its LVGL screen system — smolbase is infrastructure, not an app suite.
- **LVGL support** — rejected as heavyweight (charter Q8); consumers may add it themselves.
- **Weather-city-driven timezone mode** (reference `mode 0`) — manual IANA/POSIX selection only.
- **PSRAM support** — the target hardware has none.
- **Copying SmolTV-Pro source** — reference-only, per the brief.
