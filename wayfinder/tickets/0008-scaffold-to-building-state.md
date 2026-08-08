---
id: 8
title: Scaffold the repo to a building state
labels: [wayfinder:task]
status: open
assignee:
blocked-by: [0007-architecture-and-boot-design.md]
---

## Question

Stand up the PlatformIO project so `pio run` succeeds: `platformio.ini` (pioarduino platform pin, `esp32dev`, chosen library pins), partitions file copied from the reference, directory layout per the architecture decision, `lv_conf`-free display stack init compiling, LittleFS `data/` pack step (gzip) wired as a pre-build script (Python via uv, per global tooling rules), stub modules with the agreed boundaries, and a smoke build.

AFK task — nothing to decide; unblocks the build slices currently in the fog.
