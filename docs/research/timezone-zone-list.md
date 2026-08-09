# Research: IANA → POSIX TZ zone list source

Ticket: `wayfinder/tickets/0004-timezone-zone-list.md`
Date: 2026-08-08

## Question

The Settings UI needs a dropdown of IANA timezone names (e.g. `Africa/Johannesburg`) that map to POSIX TZ strings applied on the ESP32 via `setenv("TZ", ...)` / `tzset()`. Where should that mapping come from, how big is it, and what are the license/staleness implications?

## Chosen source: `nayarsystems/posix_tz_db`

<https://github.com/nayarsystems/posix_tz_db> — MIT-licensed repo providing `zones.csv` and `zones.json`, a machine-readable mapping of IANA zone names to proleptic POSIX TZ approximation strings (e.g. `"Africa/Cairo": "EET-2EEST,M4.5.5/0,M10.5.4/24"`), generated from the system tzdata by the included `gen-tz.py`.

Why this one:

- **It is the de-facto standard for exactly this use case.** IANA's own tz-link page (<https://data.iana.org/time-zones/tz-link.html>) lists `posix_tz_db` under "Other TZif readers" as "Python code to generate CSV and JSON tables that map tz settings to proleptic TZ approximations", noting its MIT license. That is as close to a primary-source endorsement as this niche gets, and it is the mapping most ESP32/ESPHome-adjacent projects use.
- **No better alternative surfaced.** Options like `TimeZoneConverter.Posix` (.NET) target other ecosystems; ESPHome computes POSIX strings at build time from Python `tzdata`, which doesn't apply to a runtime dropdown. Nothing else offers a maintained, permissive, ready-made JSON table.
- **Regenerable.** If freshness ever matters, `gen-tz.py` regenerates the table from any current `/usr/share/zoneinfo` — no dependence on upstream cutting a release.

## Size (measured 2026-08-08, current master)

| Asset | Raw | gzip -9 |
|---|---|---|
| `zones.json` (461 zones) | 15,581 B | 4,044 B |
| `zones.csv` (461 zones) | 15,118 B | 4,016 B |

Against a ~3.7 MB LittleFS partition the full list costs **~0.4% raw, ~0.1% gzip'd**. A curated subset is **not warranted** — it would save nothing meaningful and would break the dropdown for users outside the curated regions.

## Recommended asset format

Ship the **full `zones.json`, pre-gzipped, on LittleFS** (e.g. `/www/zones.json.gz`, ~4 KB) and serve it with `Content-Encoding: gzip` like the rest of the web assets. The Settings UI fetches it once, populates the dropdown from the keys, and on save the firmware stores both the IANA name (for display/round-tripping) and the POSIX string (what actually gets passed to `setenv("TZ")`). This keeps the firmware itself free of any timezone table and lets Consumers replace the file without recompiling.

Prefer JSON over CSV: same size gzip'd, and the browser parses it natively.

## Staleness risk

- tzdata has no fixed schedule but typically releases "every few months" (IANA tz-link, above). Most releases only touch historical data or one or two zones' future DST rules; the proleptic POSIX string for a zone changes only when its *current/future* rule changes.
- `posix_tz_db` tracks releases roughly annually (commit history: `2023c-2` Jul 2023, `2024a-1` Apr 2024, `2025b-1` Apr 2025 — GitHub commits API, retrieved 2026-08-08).
- **For a template firmware this is acceptable.** A months-stale table means at worst a recently-legislated DST change in one country is wrong until the Consumer refreshes `zones.json` (a file swap, no reflash of code if assets are OTA-updatable). Known systemic caveat: POSIX strings are proleptic approximations — they encode only the current recurring rule, not historical transitions, which is exactly what `tzset()` on ESP-IDF (newlib) consumes anyway.
- Recommend documenting "refresh `zones.json` from posix_tz_db when rebuilding" in the template's release checklist rather than any runtime update mechanism.

## License / attribution

- **`posix_tz_db`: MIT.** Embedding `zones.json` in the firmware image/filesystem requires retaining the copyright notice and license text. Add the MIT notice (Copyright Nayar Systems) to the project's third-party notices file / `LICENSES` section; no source-availability or copyleft obligations.
- **Underlying tzdata: public domain** (IANA tz-link page), so no additional obligation flows from the data itself.

## Sources

- <https://github.com/nayarsystems/posix_tz_db> — repo, README, MIT license, zone count (retrieved 2026-08-08)
- <https://api.github.com/repos/nayarsystems/posix_tz_db/commits> — release cadence (retrieved 2026-08-08)
- <https://data.iana.org/time-zones/tz-link.html> — tzdata public-domain status, release cadence, listing of posix_tz_db (retrieved 2026-08-08)
- Local measurement: `curl` of raw `zones.json`/`zones.csv` from master + `gzip -9` (2026-08-08)
- <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system_time.html> context: ESP-IDF applies timezones via `setenv("TZ")`/`tzset()` POSIX strings
