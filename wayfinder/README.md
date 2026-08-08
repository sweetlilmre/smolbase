# Wayfinder — local-markdown tracker conventions

No external issue tracker is configured for this repo, so the wayfinder map lives here as markdown.

## Operations

- **Map**: `wayfinder/map.md` (frontmatter label `wayfinder:map`). The index — never restates decisions, only gists and links.
- **Tickets**: `wayfinder/tickets/NNNN-<slug>.md`, children of the map. A ticket's **name is its title**; refer to tickets by name, linking the file.
- **Labels / type**: frontmatter `labels:` carries `wayfinder:<research|prototype|grilling|task>`.
- **Claiming**: set frontmatter `assignee:` before any work. Open + unassigned = unclaimed.
- **Blocking**: frontmatter `blocked-by:` lists ticket file names (body convention — no native blocking in markdown). A ticket is unblocked when every listed ticket has `status: closed`.
- **Frontier query**: tickets where `status: open`, `assignee:` empty, and every `blocked-by:` entry closed.
- **Resolving**: append a `## Resolution` section to the ticket, set `status: closed`, add one line to the map's *Decisions so far*.
