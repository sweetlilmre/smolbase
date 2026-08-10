# 0002 — Choice settings persist as a flat value + `<key>_name` label pair; big catalogs live browser-side

**Status**: accepted (2026-08-10)

## Context

Some settings are picked from a catalog: the user chooses by label (an IANA
zone name), the machine consumes a value (a POSIX TZ rule). The label→value
map is not reversible (many IANA names share one POSIX string), so the value
alone cannot reconstruct the user's pick — both halves must persist. The
timezone shipped as two independent string settings plus a hardcoded
`key === "tz"` hide in settings.html, which worked but put the pairing
knowledge in one skin's JavaScript. Wayfinder ticket #57 promoted it to a
first-class `Choice` setting type.

Two shapes were considered for persistence: a single key holding a nested
`{label, value}` object, or a pair of flat keys. And two homes for the
catalog: parsed by the firmware, or fetched by the browser.

## Decision

A Choice setting persists as **two flat keys**: the registered key holds the
machine value; a derived `<key>_name` key holds the label. The settings
registry keeps its flat-keys-only rule, and pre-existing `settings.json`
files (`tz` + `tz_name`) are already in this exact shape — migration is zero.

Catalogs are `{label: value}` maps and come in two sizes: **inline** (a
static `SettingChoice` array in flash, validated on `applyJson`, label
derived firmware-side from the authoritative catalog) or **by URL** (a served
asset like the 4 KB-gz `/zones.json`, fetched and resolved by the **browser**;
the firmware never parses it and trusts the posted pair as-is). The JSON
contract carries `type:"choice"`, `value`/`valueLabel`, and either `options`
or `optionsUrl`; skins POST `{key: value, key_name: label}` together.

## Consequences

- `Clock` keeps reading `getString("tz")` unchanged; nothing firmware-side
  ever parses zones.json — the browser-side lookup that motivated ticket #5
  is preserved.
- The settings UI renders every choice generically; the tz special cases in
  settings.html are gone.
- URL-sourced choices cannot be firmware-validated — a hostile client can
  store any string pair. That is exactly the trust level the tz flow always
  had; inline catalogs, the common consumer case, are validated.
- The `<key>_name` suffix is a reserved convention: a consumer must not
  register a setting whose key collides with another setting's derived label
  key.
