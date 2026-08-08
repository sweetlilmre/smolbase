# 0001 — All consumer code runs single-threaded on core 1

**Status**: accepted (2026-08-08)

## Context

System events originate on core 0 (WiFi stack callbacks, SNTP sync, PsychicHttp handlers). The template could invoke consumer hooks directly from those contexts, but then consumer code would run concurrently with its own `loop()`/`tick()` on core 1, exposing every consumer app to data races on shared state — the class of bug least diagnosable by the template's audience.

## Decision

Every consumer-facing hook — `setup()`, `loop()`, `Screen::tick()`, touch handlers, `onSystemEvent()`, settings-change notifications — executes on core 1, dispatched from the main loop. Events raised on core 0 are posted to a FreeRTOS queue and drained by the main loop; nothing in `src/core/` ever calls into `src/app/` from any other context.

## Consequences

- Consumer code is single-threaded by construction: plain variables, no mutexes, unless the consumer deliberately spawns a task.
- Event delivery incurs up to one main-loop iteration of latency (a few ms, bounded by the ~25 ms loop budget) — irrelevant for lifecycle events.
- Core 0 carries the network/system load (WiFi, httpd task, AP-mode DNS pump); core 1 belongs to the App. This split is also the performance model: app work never contends with the radio.
- Consumer HTTP route handlers are the one documented exception: they run on the httpd task (core 0) and must treat shared state accordingly — the docs must call this out.
