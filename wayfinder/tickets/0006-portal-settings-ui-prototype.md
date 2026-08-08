---
id: 6
title: Portal and Settings UI prototype
labels: [wayfinder:prototype]
status: open
assignee:
blocked-by: []
---

## Question

How should the Captive Portal and Settings UI look and behave? Build throwaway static HTML/CSS/JS pages (vanilla, no tooling — same constraints as production assets) to react to:

- Portal: scan list presentation, join flow, connect-progress/failure feedback given the AP will drop when the device switches to STA
- Settings: layout for the seven MVP settings (timezone dropdown, NTP server, brightness, hostname, WiFi re-configure, OTA upload, factory reset), save feedback, danger-zone treatment for reset/OTA
- Shared look: how small can the shared CSS be; dark/light; works on a phone (portal) and desktop (settings)

HITL — resolve via /prototype; link the prototype assets from this ticket.
