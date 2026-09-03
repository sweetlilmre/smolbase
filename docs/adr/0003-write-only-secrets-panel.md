# 0003 — Secrets get a write-only Settings UI panel driven by declared descriptors and the existence map

**Status**: accepted (2026-08-10)

## Context

The Secret Store (ticket #23) deliberately sits outside the Settings Schema:
values never auto-render, never serialize, and `GET /api/secrets` returns an
existence map, never values. That left it with no UI at all — loading a key
meant a hand-rolled `curl`. The Weather Clock effort (map #63, ticket #66)
needs an end-user path for an OpenWeatherMap API key, and any consumer app
with a token faces the same gap.

Three shapes were considered for how a panel learns what to render: free-form
key/value entry (the user types raw NVS key names), app-declared descriptors
(registration buys UI, like the Settings Schema), or both. And two contracts
for reading state back: keep the existence map, or start echoing values to
the browser like SmolTV-Pro does (it serves its OWM key in cleartext to
anyone on the LAN).

## Decision

Apps declare **Secret Descriptors** in code — key, label, hint — and the
stock Settings UI renders a **Secrets section** with one write-only input per
descriptor: always-blank password field with show/hide, a "set" badge derived
from the existence map, Set writes via `POST /api/secrets`, Clear posts null
after one confirm. The section is hidden entirely when no descriptors are
registered, so non-consumer builds show nothing.

The web read contract stays the existence map — extended to carry the
descriptor metadata (label, hint, set-flag) but **never values**. Replace is
"type a new value over a set key"; there is no read-back, ever. No free-form
row: an undeclared secret remains reachable by `curl`, not by UI.

## Consequences

- Registration buys UI, matching the Settings Schema philosophy: a consumer
  adds one descriptor line and ships a labeled, safe key-entry field.
- A value, once set, can only be replaced or cleared — never inspected. Lost
  keys are re-entered, not recovered; that is the point.
- The existence-map contract hardens from "current behavior" into a promise
  UIs build on: no future endpoint may echo secret values.
- Descriptors are firmware-side strings in flash; renaming a label is free, renaming a *key* orphans the stored NVS value (it stays until factory reset or a curl clear) — the descriptor key is effectively an API.
- A per-App `src/app-<name>/html/validators.js` can register client-side validators in `window.SECRET_VALIDATORS` (keyed by secret key). The Set button runs the validator and shows errors inline before posting — no firmware involvement, no round-trip. The settings page loads the file gracefully; a 404 is silently ignored.
