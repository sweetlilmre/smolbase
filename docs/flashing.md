# Flashing

The Small TV Pro's USB-C port is **power only — there is no USB-serial
bridge**. The first flash goes over a 6-pad serial header inside the case;
every flash after that is OTA over WiFi, so you open the case exactly once.

## First flash: the internal serial header

Open the case. Next to the ESP32 module is a 6-pad programming header:

**GND · TX · RX · 3V3 · GPIO0 · RST**

Wire it to any generic 3.3 V USB-UART adapter (CP2102, CH340, FTDI — set to
**3.3 V**, never 5 V logic):

| Board pad | USB-UART adapter |
| --- | --- |
| GND | GND |
| TX | RX |
| RX | TX |
| 3V3 | 3V3 (or power the board over its own USB-C and skip this pad) |
| GPIO0 | GND — only while entering boot mode (see below) |
| RST | not required; touch to GND to reset, or just power-cycle |

TX/RX cross over. If the serial monitor shows nothing at 115200, swap them —
that is the failure mode, not damage.

### Entering boot mode

The ESP32 samples GPIO0 at reset: **low = serial bootloader**.

1. Hold GPIO0 to GND.
2. Reset the board (pulse RST to GND, or power-cycle).
3. Release GPIO0. The chip is now waiting for the flasher.

No adapter with DTR/RTS auto-reset wiring is assumed — do it manually. You
must re-enter boot mode for each upload command.

### Upload

One command writes everything a fresh board needs — bootloader, partition
table, firmware, and the LittleFS image with the web assets and `zones.json`:

```
idf.py @smolbase.args -p COM5 flash
```

(In PowerShell quote the argfile: `idf.py '@smolbase.args' -p COM5 flash`. On
Linux/macOS the port is `/dev/ttyUSB0`-style. Substitute `@weatherclock.args`
or `@gcm.args` for the other Apps.)

After flashing, reset with GPIO0 released. The screen comes up, the device
starts its provisioning AP (`smolbase-XXXX`), and the captive portal gets it
onto your WiFi. Optionally watch it boot:

```
idf.py @smolbase.args -p COM5 monitor
```

## Ever after: OTA

Close the case; you never open it again. Once the device is on your network,
both images update over HTTP.

**Via the recovery page** — open `http://smolbase-XXXX.local/recover`
(hostname is on the device's screen). It is compiled into the firmware
itself, so it works even when the filesystem is empty, wiped, or broken —
upload `smolbase.bin` (firmware) or `spiffs.bin` (filesystem) from
`build/smolbase/`, both of which `idf.py build` produces. If the device boots
with no web assets at all, plain `http://<ip>/` serves this same page.

**Via the settings page** — once the filesystem image is on the device,
`http://smolbase-XXXX.local/settings.html` has an Update tab for routine
firmware updates.

**Via curl** — the scriptable alternative. The endpoint is
`POST /api/update`; the `target` query parameter picks the partition
(`fw` is the default):

```
# firmware
curl -F "file=@build/smolbase/smolbase.bin" \
     http://smolbase-XXXX.local/api/update

# filesystem (web assets)
curl -F "file=@build/smolbase/spiffs.bin" \
     "http://smolbase-XXXX.local/api/update?target=fs"
```

(The filesystem image is named after its partition, which is labelled `spiffs`
for historical reasons — the stock flash layout. It holds LittleFS.)

A successful upload answers `{"ok":true,"restarting":true}` and the device
reboots into the new image; errors come back as HTTP 400 with the reason.
`GET /api/update/status` reports progress. The partition table
(`partitions.csv`) keeps two app slots, so a firmware upload streams into the
inactive slot and only switches on success — a failed or interrupted upload
leaves the running firmware untouched.

One update at a time: a second upload racing an in-flight one is refused with
HTTP 409.

**Self-update from GitHub releases.** The settings page's Update tab also
pulls firmware straight from this repo's GitHub releases: **Check** compares
the running version against the latest release tag (`GET /api/update/check`),
and **Update** downloads and flashes it with live progress
(`POST /api/update/github {"tag":"vX.Y.Z"}`, polled via
`GET /api/update/ghprogress`). Under the hood a Core-0 task runs
`esp_https_ota` against the release asset
`<app>-firmware-<tag>.bin` — each App pulls **its own** asset
(`SMOLBASE_FW_ASSET_PREFIX`, set per App in `CMakeLists.txt`), so a
weatherclock device never flashes the smolbase image. A ~1.5 MB image takes
~25 s on a healthy WiFi link.

The self-update also carries the **web assets**: the release ships an
`<app>-assets-<tag>.tar` (the complete gzipped `/w/` set, ~30 KB) which the
device downloads first — verified against GitHub's per-asset sha256 digest —
then renames `/w` to a version-named backup and extracts before the firmware
is finalized, so firmware and assets land together in the one reboot.
`/config/settings.json` is never touched: **settings survive a self-update**
(only the full-image `?target=fs` flash resets them). Every failure or
rollback permutation heals itself: an interrupted update restores the exact
old asset set at boot, and the backup is discarded once the new image
survives the 30 s guard. When already up to date, the Update tab offers
**Reinstall assets** — same tar, applied in place, no reboot. Design and
verification: wayfinder maps #112/#121, `docs/research/assets-tar-mechanics.md`.

One gap to know about: firmware **v0.3.1 and older**
cannot verify GitHub's 2026 CDN certificate chain (RSA-4096 hardware limit,
see ADR 0005) — those devices need one manual `POST /api/update` upload of a
v0.3.2+ image, after which self-update works again.

**Boot-loop guard.** A firmware that uploads fine but crashes at boot would
otherwise boot-loop an OTA-only device with no way back in. So a freshly
flashed image boots *unconfirmed*: the firmware marks itself good only after
**30 seconds of healthy uptime**. Any crash, panic, or reset before that and
the bootloader falls back to the previous firmware on its own — the device
comes back on the old version, reachable for another upload. Side effect:
power-cycling within 30 s of an update also reverts it; just upload again.
(The serial log prints `[ota] image confirmed healthy` when the new image is
accepted. Without a UART, `GET /api/status` reaching 30 s of `uptimeS` on the
new `fwVersion` is the same evidence.)

**Dev loop — single-file upload.** Reflashing the whole filesystem for one
edited page is slow and wipes `settings.json`. `POST /api/fs?path=...` writes
one file straight into LittleFS instead. Web assets are served gzip-only from
`/w/`, so the loop for an edited page is:

```
gzip -kf9 html/settings.html
curl -F "file=@html/settings.html.gz" "http://smolbase-XXXX.local/api/fs?path=/w/settings.html.gz"
```

Refresh the browser — no reflash, no restart, settings intact. This is a dev
convenience, not a deploy mechanism: fs-OTA remains the way to ship a
coherent image.

**A filesystem update replaces the whole LittleFS partition** — including
`/config/settings.json`. Settings revert to defaults; WiFi credentials survive
(they live in NVS), so the device comes back on your network and you reconfigure
via the settings page.
