---
id: 5
title: Extension surface design
labels: [wayfinder:grilling]
status: open
assignee:
blocked-by: [0002-display-stack-selection.md]
---

## Question

What exactly does a Consumer touch when building on smolbase? Design the extension surface:

- App lifecycle hooks (setup/loop? task-based? where does consumer code live in the tree — an `app/` module?)
- App Screen contract: how a consumer replaces the Stock Screen (interface shape depends on the chosen display stack — hence blocked by *Display stack selection*)
- Registering consumer HTTP routes on the PsychicHttp server
- Extending the settings JSON schema and having fields appear in (or alongside) the Settings UI
- Subscribing to touch events (tap / long-press)
- What is *sealed* vs *forkable* — template repo model means everything is technically editable; which modules do we document as "yours" vs "plumbing"?

HITL — resolve via /grilling + /domain-modeling; update CONTEXT.md's *Extension Surface* entry with the result.
