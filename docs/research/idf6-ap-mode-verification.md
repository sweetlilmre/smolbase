# AP mode and the captive portal — verification procedure

**Written:** 2026-08-23, after the IDF 6 migration. This is the one path the port changed that nobody has exercised on the native build.
**Needs:** a phone or laptop with WiFi, the password for the network the device normally joins, and about ten minutes.

## Outcome (2026-08-23)

**Run, and it found two bugs — one of them brick-class.** Full results below; the procedure is kept because it is worth repeating after anything touches `Net.cpp` or `Web.cpp`.

The captive-portal path itself passes completely. `scripts/`-adjacent probe, from a laptop joined to the AP over a spare Wi-Fi adapter while the LAN stayed on Ethernet — 14 checks, 0 failures:

| Checked | Result |
|---|---|
| `/api/status` over the AP, `apMode: true` | pass |
| DNS hijack: 4 probe hostnames incl. a `.invalid` | all answer `192.168.4.1` |
| Foreign-host redirect: gstatic / apple / msftconnecttest probes | all `302 -> http://192.168.4.1/` |
| Self-addressed requests do NOT redirect (loop guard) | pass for `/`, `/settings.html`, `/api/status` |
| Portal page served (AP-mode rewrite of `/`) | pass, 11,407 B gzip |
| Scan results stable across repeated polls | pass |

**Bug 1 — the HTTP server could silently never start.** `Web::start()` was one-shot and lost a race against the AP netif coming up, so the device beaconed, served DHCP and answered the captive DNS with nothing on port 80: joinable, unprovisionable, and unrecoverable without the serial header. The captive DNS *working* is what pinned it, because `Portal::begin()` runs on the line after `Web::start()` in the same handler. Fixed by retrying until a netif is up; confirmed on the wire as four failures at 250 ms then success the moment the IP landed.

**Bug 2 — the first scan could report "done" with zero networks**, which the portal renders as "No networks found. Try a rescan." Three disparities against the arduino-esp32 scan path, the main one being that Arduino collected the records inside the `SCAN_DONE` handler while this port deferred the fetch to the next HTTP poll.

**Also measured, and worth not re-deriving:** the STA re-associates exactly once per boot, 5–15 s after connecting, back with the same IP in ~200 ms. Not power save (verified with `WIFI_PS_NONE`), not any WiFi call of ours. mDNS now rides through it rather than rebuilding.

**A serial adapter is worth attaching before running this.** Bug 1 made the device unreachable, and the internal header was the only way back. With serial attached the whole loop — flash, reset, read the boot log — takes seconds, and the UART is the only place `psychic: Server start failed` was ever visible.

## Why this needs a human

Everything else in the migration was verifiable over HTTP from the dev host. This is not: the captive-portal behaviour is a conversation between a *client's operating system* and the device, and the interesting parts (does the portal pop unprompted, does a foreign hostname get redirected) only happen when a real client joins the AP. The dev host cannot join the device's AP without leaving the network it needs to report from.

## Risk, stated plainly

The bench has **no serial flasher and no UART reader**. If AP mode is broken badly enough that the HTTP server never starts, the device becomes unreachable and the only way back is opening the case and wiring a 3.3 V USB-UART to the internal header ([docs/flashing.md](../flashing.md)).

That risk is real but small, and here is the honest accounting:

- AP entry, the portal, and re-provisioning were confirmed once on this branch already — accidentally, by an errant `/api/wifi/forget` — but that was before phase 7c, on the arduino-esp32 build.
- Since 7c the things in this path that changed are `serveStatic` (now PsychicHttp's native `psychic::FS`) and the `uri()` comparisons in `onNotFound` (now `strcmp`, because native mode returns `const char*` and `==` compared pointers).
- Both netifs are created up front in `Net.cpp` (phase 6c), which is what `ON_AP_FILTER` needs, and the STA half of that filter logic is already verified — see below.
- `/recover` is a firmware route, compiled into `.rodata`, registered before static assets and served in every mode. If httpd starts at all, there is a way in.

## Already verified, so don't re-test these

Two of these were confirmed by accident while chasing the filesystem-image bug, which is worth more than a deliberate test:

| Behaviour | How it was confirmed |
|---|---|
| `onNotFound` STA branch serves the embedded recovery page | With `/w` empty after a bad fs image, `GET /settings.html` returned the 2,446-byte compiled-in page instead of a 404 |
| `ON_AP_FILTER` does **not** match on the STA netif | `GET /` in STA mode served `index.html` (2,129 B), not `portal.html` — the AP-mode rewrite correctly stayed inert |
| Static serving from LittleFS through native `psychic::FS` | All six assets byte-identical to the built image, gzip fallback included |
| mDNS name + service discovery | Direct mDNS query answered `A`, `PTR _http._tcp`, `SRV :80` |

## Procedure

### 0. Before you start

Have the WiFi password to hand. Record the current settings if you care about them — a re-provision keeps `settings.json` (it lives on LittleFS, and only a full `?target=fs` flash resets it), so this should be lossless, but check:

```
curl http://smolbase-2e00.local/api/settings
```

### 1. Drop to AP mode

```
curl -X POST http://smolbase-2e00.local/api/wifi/forget
```

The device clears its stored credentials and restarts.

**Check:** the panel shows the AP info screen — an SSID of the form `smolbase-2e00` and connection instructions. If the panel stays on the clock, or goes black, stop and report that.

### 2. Join the AP

Join `smolbase-2e00` from a phone. No password.

**Check — and this is the main event:** the captive portal should appear **on its own**, without you opening a browser. That single behaviour exercises three things at once:

- the AP-mode DNS responder answering every lookup with the AP's own IP (our own responder now, replacing Arduino's `DNSServer` — phase 6b, never tested)
- `onNotFound` recognising the OS probe's hostname as foreign and answering `302` to `http://<ap-ip>/`
- `rewrite("/", "/portal.html")->setFilter(ON_AP_FILTER)` matching on the AP netif

If it does not pop by itself, browse to `http://192.168.4.1/` manually and note that it needed manual navigation — that narrows it to the DNS responder or the redirect rather than the portal itself.

### 3. Confirm the portal, not a redirect loop

**Check:** the portal page renders (scan-and-join UI, not the minimal fallback — the fallback says "fallback page — upload the filesystem image after joining", and seeing it would mean `/w/portal.html` is missing).

**Check:** the page's asset requests succeed rather than looping. A request already addressed to the AP IP must **not** be redirected — that is the guard against an infinite browser loop, and it is the `selfAddressed` test in `onNotFound`. A visibly broken or endlessly reloading page points there.

### 4. Confirm the scan

**Check:** the network list populates within a few seconds and shows your networks with signal strengths. This exercises AP_STA mode (the AP must stay up while the STA radio scans) and the result handoff — `esp_wifi_scan_get_ap_records` consumes the driver's results, so they are collected in the `SCAN_DONE` handler and served from `scanHits` afterwards. **Leave the page a moment and let it poll more than once:** a list that appears and then goes empty, or a `done` with no networks at all, is that handoff broken — which is exactly bug 2 above.

### 5. Re-provision

Pick your network, enter the password.

**Check:** the page reports the device is restarting to join. The device reboots, joins, and comes back at its usual address.

```
curl http://smolbase-2e00.local/api/status
```

**Check:** `apMode` is `false`, `wifi.ssid` is your network, and the settings from step 0 are still there.

### If step 5 fails

You are in AP mode with a working HTTP server, which is recoverable. Join the AP and either use the portal again, or go headless:

```
curl -X POST http://192.168.4.1/api/wifi \
     -H "Content-Type: application/json" \
     -d '{"ssid":"YOUR-SSID","pass":"YOUR-PASSWORD"}'
```

`http://192.168.4.1/recover` is compiled into the firmware and works even with no assets on the filesystem, so it is also available for a firmware or filesystem re-upload.

## What to report back

For each numbered step: passed, or what happened instead. The distinction that matters most is **step 2**: "portal popped by itself" versus "I had to browse to the IP" versus "the IP did not answer" — those are three different faults in three different pieces of code.
