# ESP-IDF 6 migration — Phase 0 report

**Date:** 2026-08-22
**Verdict:** Phase 0 complete. **12 of 12 checks pass on real hardware.** Nothing found argues against migrating, and the load-bearing claim behind it is now proven rather than assumed.
**Companions:** [esp-idf-6-migration.md](esp-idf-6-migration.md) (the plan) · [littlefs-wrapper-sketch.md](littlefs-wrapper-sketch.md) (the `sb_fs` design) · [`spike/idf6/`](../../spike/idf6/) (the code)

## What Phase 0 was for

The migration assessment rested on four arguments and half a dozen "should work" assumptions. Phase 0 existed to convert those into yes/no answers before committing three weeks. It ran in two halves: desk verification against primary sources, then a throwaway ESP-IDF 6.0.2 firmware flashed OTA to the live device.

## The headline result

**`CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` works as a plain `sdkconfig` line.** Check 8 fetched a real GitHub release asset, followed the 302 to the Fastly CDN, and verified its chain — the one cross-signed by RSA-4096 ISRG Root X1 that issue #119 is about:

```
PASS  8 tls cdn rsa4096   v0.3.3 http 206, 8192 B, floor 143236, err=ESP_OK
```

This is the whole thesis of the migration. ADR 0005's hybrid compile exists **only** because pioarduino ships precompiled IDF libraries, so those mbedTLS knobs can be changed only by rebuilding them locally — a ~15 minute rebuild, a fragile CI cache key, and a PowerShell-only build on Windows. Under native IDF everything compiles from source and the knobs are three ordinary lines. That is now measured, not argued.

## All twelve results

Run 5, ESP-IDF v6.0.2, flashed OTA to the live device:

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | NVS credentials | PASS | Arduino `Preferences` strings read by raw `nvs_get_str`. **Fielded devices need no re-provisioning.** |
| 2 | LittleFS `/w` | PASS | 6 files, 2 dirs at root, 56/3776 KB. Non-zero dir count also proves `d_type` is populated. |
| 3 | Panel init | PASS | 240x240, LovyanGFX 1.2.27 unmodified |
| 4 | Band push (DMA) | PASS | **6185 µs against a ~6100 µs theoretical bus floor — 1.4% over.** SPI DMA at 40 MHz is fully intact. |
| 5 | Touch T9 | PASS | SMOOTH baseline 1607 across 16/16 reads. `RAW=ESP_ERR_INVALID_STATE` — see the tracked upstream item. |
| 6 | WiFi join | PASS | `esp_wifi` + `esp_netif` + `esp_event`, rssi -58 |
| 7 | TLS api.github.com | PASS | http 200, `esp_crt_bundle` |
| 8 | **TLS CDN RSA-4096** | **PASS** | **http 206, 8192 B** — the headline above |
| 9 | PsychicHttp native | PASS | Non-Arduino mode; these results were served through it |
| 10 | Footprint | PASS | free 165 924, largest block 110 592, min-ever 131 152, TLS floor 143 236, main task 6408 B stack free of 8192 |
| 11 | `sb_fs` wrapper | PASS | ArduinoJson round-trip through the RAII handle; path ops correct both ways |
| 12 | Root mount | PASS | **`"/w"` resolves unprefixed** |

## What changed in the plan because of this

**Two arguments got stronger.**

- *ADR 0005 dies* — now proven end-to-end, not inferred.
- *No downstream forks yet* — this remains the strongest timing argument. The migration is free of consumer cost exactly once and that window is open now.

**One argument got demoted.** Heap headroom. The raw delta looks like +32 KB (165 924 vs the Arduino `smolbase` env's 133 560), but the spike carries a 30.7 KB band scratch where `smolbase` carries a 57.6 KB framebuffer, so it starts ~26.9 KB ahead on static alone. Like-for-like that is **roughly 5–10 KB**, and the spike runs no app, no settings registry, no `Clock`, no `AssetUpdate`, and never starts mDNS. Treat it as an order of magnitude, not a measurement. **Heap is a footnote, not a headline** — the doc previously implied "meaningful recovery" and that was wrong.

**One design decision reversed.** The `sb_fs` sketch recommended editing six path constants because Arduino's LittleFS paths are volume-relative while POSIX needs the mount point. Check 12 shows a root mount (`base_path = ""`) works and `/w` resolves unprefixed, so **every existing path constant stays byte-identical** — no prefixing layer, no constant edits, no second path namespace colliding with PsychicHttp's real POSIX paths.

**Three concrete findings for the port.**

- `VSPI_HOST` is removed in IDF 6. `Display.cpp:22` uses it; `SPI3_HOST` is the same peripheral on classic ESP32, so a rename.
- **Touch thresholds need re-calibrating.** The new driver's SMOOTH baseline is ~1607, while `Touch.cpp`'s constants (`DEFAULT_THRESHOLD` 300, `MIN_BASELINE` 200, `MARGIN` 120) were tuned against Arduino's `touchRead()`. Different scale entirely — this needs a bench pass the plan did not previously account for.
- IDF builds with `-Werror=missing-field-initializers` and `-Werror=format=`, which the Arduino build does not enforce. A small tax across 20 000 lines.

## Tracked: the touch RAW bug is a stale pin, not a live defect

`hw_ver1/touch_version_specific.c:253` in v6.0.2 guards reads with `type == SMOOTH && filter != NULL` where it means `type != SMOOTH || filter != NULL`, so every `TOUCH_CHAN_DATA_TYPE_RAW` read returns `ESP_ERR_INVALID_STATE`. Confirmed empirically by check 5.

It was **already reported and fixed**: [espressif/esp-idf#18811](https://github.com/espressif/esp-idf/issues/18811) (IDFGH-17938), raised 2026-07-09, confirmed by an Espressif engineer the next day, closed Done 2026-07-10. `master` and `release/v6.0` both carry the corrected form; the fix landed after the v6.0.2 tag.

**Action:** no local patch, and nothing to report upstream. The workaround (`touch_sensor_config_filter()` with `data_filter_fn = NULL`, then read SMOOTH) is valid on *both* the buggy and fixed guards, so it is forward-compatible and costs nothing to keep. Pin the latest patch release at Phase 7; if it is v6.0.3+, `RAW` works and `Touch.cpp` ports nearly verbatim.

## Build times

Measured, not estimated:

| Operation | Time |
|---|---|
| no-op `idf.py build` | 3.3 s |
| no-op `ninja -C build` | 1.1 s |
| one-file edit (`ninja`) | 24.1 s |
| one-file edit (`idf.py`) | 26.0 s |
| clean rebuild, **cold** ccache | ~10 min |
| clean rebuild, **warm** ccache | **177 s** (983/985 hits, 99.8%) |

`idf.py`'s Python and CMake front matter costs only ~2 s, so it is not worth bypassing. A one-file edit dirties just **10 build edges**, and essentially all 24 s is one translation unit plus the link: `main.cpp` pulls in `LovyanGFX.hpp`, PsychicHttp and ArduinoJson, all heavy C++ template headers, and then a 1 MB image links.

Two things worth knowing:

- **The spike is a pathological case.** It is one giant translation unit. The real firmware has ~40 small ones, so its incremental builds will be far cheaper than these numbers suggest — this is not a preview of day-to-day migration cost.
- **The ~10 minute cold build is the tradeoff being bought, not a regression.** Native IDF compiles the whole SDK — mbedTLS, lwIP, the WiFi stack — from source. That is *precisely* the property that makes the mbedTLS knobs reachable and ADR 0005 dissolve. pioarduino's cold build is faster because it ships precompiled libs, which is the thing causing the problem in the first place.

**ccache is the fix for the one that hurts.** It ships with the IDF tools (4.12.1) and `IDF_CCACHE_ENABLE=1` is already set by `export.ps1`. Once warm it takes a clean rebuild from ~10 min to **177 s at 99.8% hit rate** — a 3.4x cut, which matters because clean rebuilds are what a branch switch or an `sdkconfig` change costs. Nothing to configure; it just needs to have been used once.

So the honest build-cost picture for the migration: **~25 s to iterate, ~3 min to rebuild from clean.** That is unremarkable, and better than the current hybrid-compile situation where a `platformio.ini` change triggers a ~15 minute IDF-library rebuild.

A Defender exclusion for the build tree and `~/.espressif` would help further but needs admin rights — a question for IT rather than something to change unilaterally.

## The spike's safety design, which held

The device has **no serial flasher**, so a bad image had to be recoverable over the air. Two properties made five flash cycles safe:

1. **The spike never calls `esp_ota_mark_app_valid_cancel_rollback()`.** It runs permanently as `ESP_OTA_IMG_PENDING_VERIFY`, so any reset returns the device to fw 0.3.3. Verified: the running bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` with anti-rollback off, and that is the bootloader that decides, since `/api/update` writes only the app partition.
2. **A `/restart` route**, added after run 1 cost a physical power cycle. A restart route — not an update route — is the right primitive: the image is always PENDING_VERIFY, so `esp_restart()` makes the bootloader roll back and the real firmware's `/api/update` takes over. An update route on the spike would instead write into the slot holding the real firmware and destroy the rollback target. Rollback measured at ~5 s.

Results left the device by panel and HTTP rather than UART, for the same reason.

## Process notes worth keeping

Three of the five runs were spent on my own errors, and two of those were avoidable by looking rather than reasoning.

- **Check 8 took three attempts.** A nonexistent asset name (which produced a *false pass* on github.com's own 404 — the worst outcome, since it looked like success), then hand-driving the redirect, then `perform()`. The real cause was `buffer_size_tx` defaulting to 512 bytes while GitHub's signed CDN URL is ~1.2 KB. **That fix was already in this repo**, commented in `GhUpdate.cpp:163` and `AssetUpdate.cpp:149`: *"request line must fit the ~1.2 KB CDN redirect URL"*. Reading the working implementation of the thing being reimplemented should have been step one.
- **The touch bug was diagnosed correctly but framed wrongly** — presented as an undiscovered defect worth reporting when one `gh search issues` showed it already fixed.
- **A pass condition that accepts too much is worse than a failure.** Check 8's original `status > 0` turned a wrong URL into a green tick on the single most important claim. Assertions on critical checks should demand the specific evidence (`206` *and* non-zero bytes), not merely absence of error.
- **Do not zero-initialise IDF config structs.** `touch_channel_config_t chanCfg = {}` set `charge_speed` to enum value 0 — the slowest, not "unset" — so the pad never charged and the filter faithfully smoothed a stream of zeros.

## Recommendation

Phase 0 clears the way. The remaining plan stands as written in [esp-idf-6-migration.md](esp-idf-6-migration.md): 12–18 working days, ~8 mergeable PRs, of which only the last changes the framework, with every earlier phase landing on the Arduino build and flashable in isolation.

Two things to do regardless of whether the migration proceeds:

1. **Phase 1 — gate the OTA rollback confirm on network-up.** `Ota::tickRollbackGuard()` currently confirms on 30 s of uptime alone, so an image that boots but never joins WiFi marks itself valid and strands an OTA-only device. Independent of this migration, and worth doing this week.
2. **Delete `spike/idf6/` once its findings are absorbed.** It is throwaway by design, and `sb_fs.h` is the only part meant to survive — into `src/core/` at Phase 5.
