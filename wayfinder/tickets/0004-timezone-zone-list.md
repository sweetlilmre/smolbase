---
id: 4
title: Curated IANA → POSIX TZ zone list source
labels: [wayfinder:research]
status: open
assignee:
blocked-by: []
---

## Question

The settings UI offers a dropdown of IANA zone names mapping to POSIX TZ strings (applied via `setenv("TZ")/tzset()`). Where does that list come from?

- Is there a maintained, permissively-licensed machine-readable mapping (e.g. the well-known `zones.csv`/`zones.json` derived from tzdata used by ESP32 projects)?
- How large is the full list as a gzip'd JSON asset, and is a curated subset warranted?
- How stale can the mapping get (tzdata churn) and does that matter for a template?
- License/attribution requirements for embedding it.

Deliverable: chosen source with license note, plus the recommended asset format for LittleFS.
