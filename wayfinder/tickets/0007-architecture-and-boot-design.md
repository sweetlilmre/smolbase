---
id: 7
title: Module architecture and boot/network state machine
labels: [wayfinder:grilling]
status: open
assignee:
blocked-by: [0002-display-stack-selection.md, 0003-psychichttp-capabilities.md, 0005-extension-surface-design.md]
---

## Question

Design smolbase's module decomposition and runtime model — the last decision layer before scaffolding:

- Module boundaries and dependency direction (config store, wifi/provisioning, web, time, display, touch, ota, app) — deep modules, minimal headers
- Task model: what runs on FreeRTOS tasks vs the Arduino loop; core pinning; where PsychicHttp's task sits
- Boot/network state machine: boot → try stored creds → STA connected ⇄ AP Fallback; concrete connect timeout, runtime-loss detection window, retry/backoff policy (Q13 left these open)
- Memory policy: static buffers over heap (reference precedent: keep heap contiguous for TLS), buffer sizes
- Config store API shape (split NVS/LittleFS decided; define the read/write/subscribe API and the JSON layout)
- Directory layout of the template repo

HITL — resolve via /grilling + /domain-modeling; record hard-to-reverse choices as ADRs.
