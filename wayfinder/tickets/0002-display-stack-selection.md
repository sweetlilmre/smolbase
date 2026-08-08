---
id: 2
title: Display stack selection
labels: [wayfinder:research]
status: open
assignee:
blocked-by: []
---

## Question

LVGL is rejected as too heavyweight for this template. Which modern, highly-recommended, high-performance display stack should smolbase use for the ST7789V 240×240 SPI panel on a no-PSRAM classic ESP32 (arduino-esp32 3.x / IDF 5.x, PlatformIO + pioarduino)?

Candidates to evaluate at minimum: **LovyanGFX**, **Arduino_GFX (moononournation)**, **TFT_eSPI**, and anything newer with strong traction. Criteria:

- Raw SPI throughput on classic ESP32 (DMA support that actually works on IDF 5.x — note TFT_eSPI's DMA is reportedly broken there)
- Flash/RAM footprint with no PSRAM (static vs heap buffers)
- Active maintenance and arduino-esp32 3.x compatibility
- API ergonomics for template consumers (text, primitives, sprites/off-screen buffers)
- ST7789 240×240 support quality (the CS-tied-low quirk, rotation offsets)

Deliverable: a recommendation with evidence, and the known-good init parameters for this panel.
