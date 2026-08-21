# Wayfinder — tracker location

Wayfinder maps live on GitHub Issues, label `wayfinder:map`:

- [Smolbase — MVP template firmware for the Small TV Pro (#1)](https://github.com/sweetlilmre/smolbase/issues/1) — complete.
- [Weather Clock — port the SmolTV-Pro weather dashboard as a second App (#63)](https://github.com/sweetlilmre/smolbase/issues/63) — complete (merged via PR #79); field-finding follow-ups #77/#78/#80 remain open as ordinary tickets.
- [OTA self-update from GitHub releases (#106)](https://github.com/sweetlilmre/smolbase/issues/106) — feature shipped (firmware-only); map still open.
- [GhUpdate deep review & memory optimization (#112)](https://github.com/sweetlilmre/smolbase/issues/112) — complete (merged via PR #120): esp_https_ota rewrite, hybrid-compile software-MPI/TLS-buffer fixes, per-env release assets; verified on-device.
- [Incremental filesystem updates over GitHub OTA (#121)](https://github.com/sweetlilmre/smolbase/issues/121) — in progress: design settled (#122), research + implementation pending.

**Tickets**: child (sub-)issues of the map, labelled `wayfinder:<research|prototype|grilling|task>`.

## Wayfinding operations

- **Claiming**: assign the issue to yourself before any work. Open + unassigned = unclaimed.
- **Blocking**: native GitHub issue dependencies (*blocked by*). Unblocked = every blocking issue closed.
- **Frontier query**: open, unblocked, unassigned child issues of the map — e.g. `gh issue list --label "wayfinder:*" --no-assignee` then check each issue's *blocked by* relationships.
- **Resolving**: post the answer as a resolution comment, close the issue, append a one-line gist + link to the map's *Decisions so far*.
- **Research findings**: land on throwaway `research/<name>` branches as `docs/research/<name>.md`, linked from the ticket's resolution comment.
