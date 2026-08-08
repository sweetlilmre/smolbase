---
id: 3
title: PsychicHttp capabilities and version choice
labels: [wayfinder:research]
status: open
assignee:
blocked-by: []
---

## Question

Smolbase will use PsychicHttp. Which version (v1 vs v2), and what do its capabilities mean for our design? Establish, from primary sources (repo, docs, issues):

- v1 vs v2 status: stability, arduino-esp32 3.x / IDF 5.x compatibility, PlatformIO registry availability
- Serving gzip'd static files from LittleFS with `Content-Encoding: gzip` (built-in or manual headers?)
- Captive-portal fit: wildcard/catch-all routes, redirect handling, coexistence with DNSServer
- File upload + OTA handling (multipart upload handlers, Update.h integration patterns)
- WebSocket/EventSource support (relevant to the consumer extension surface, not MVP-critical)
- Memory characteristics vs raw `esp_http_server` (task stack, per-connection cost, recommended `max_open_sockets` on a no-PSRAM ESP32)

Deliverable: version pin recommendation and a capability map with any gotchas.
