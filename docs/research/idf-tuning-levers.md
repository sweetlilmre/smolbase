# IDF tuning levers — measured, none applied

**Written:** 2026-08-23, during the phase 9 cleanup, against ESP-IDF v6.0.2 and the smolbase App.
**Status:** measurement only. Nothing in this document has been applied to `sdkconfig.defaults`. Each lever is listed with what it actually costs and what it actually risks, so the decision is a decision rather than a guess.

## Why the framing matters

Flash has **35% of the app partition free** (1.46 MB of 2.17 MB used). Heap does not: the binding constraint on this chip has always been RAM, and specifically the 8-bit-accessible pool a TLS handshake draws from. So a lever that buys flash buys nothing that is currently scarce, and a lever that buys heap is worth real risk.

The IDF 6 migration already moved the number that matters — free 8-bit heap went 77.6 KB → 124.3 KB ([ADR 0006](../adr/0006-native-esp-idf-framework.md)). What follows is what is left.

## Method, and two traps that invalidate it silently

Each variant is a full build into its own directory with an overlay on top of the tracked defaults:

```
idf.py -B build/tune-<v> -D SMOLBASE_APP=smolbase \
       -D SDKCONFIG=build/tune-<v>/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;<overlay>.defaults" build
```

**Trap 1: the generated `sdkconfig` shadows the defaults.** `SDKCONFIG_DEFAULTS` is only consulted when the target `sdkconfig` does not already exist. Without `-D SDKCONFIG=` pointing into the variant's own build directory, every variant reuses the project-root `sdkconfig` from a previous build and the overlay is silently ignored — the first attempt at this produced a cert-bundle "measurement" byte-identical to baseline, which is what gave it away. (This is also why the argfiles now set `SDKCONFIG=build/<app>/sdkconfig`: nothing is shared between Apps, and a stale root file cannot override the tracked defaults.)

**Trap 2: a Kconfig `choice` needs the current pick explicitly unset.** Adding `CONFIG_..._CMN=y` after `CONFIG_..._FULL=y` leaves FULL winning. The overlay must contain both:

```
# CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL is not set
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y
```

Always assert the override landed in the generated `sdkconfig` before believing a number.

## Baseline (smolbase App, v6.0.2)

| | bytes |
|---|---|
| Flash Code | 834,872 |
| Flash Data | 515,612 |
| DRAM (`.data` + `.bss`) | 104,040 (57.6% of 180,736) |
| IRAM | 88,015 (67.2% of 131,072) |
| Total image | 1,456,723 |

On-device, for comparison: free heap 166.5 KB, free 8-bit 124.3 KB, largest 8-bit block 110.6 KB.

## Measured

| Lever | Flash | DRAM | Verdict |
|---|---|---|---|
| `MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` instead of `_FULL` | **−50,792** | 0 | Real, and the only large flash lever left. Buys nothing scarce. |
| WiFi buffers: static RX 8→6, dynamic RX 32→16, dynamic TX 32→16 | **0** | **0** | Byte-identical output. See below — this is a finding about the *tool*, not the lever. |

### The cert bundle: 50 KB of flash, and the reason not to take it

`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` embeds the full root set; `_CMN` embeds the common subset. The saving is real and costs no heap.

The risk is asymmetric and lands on someone else. This is template firmware: a consumer points it at their own weather provider, their own CGM endpoint, their own webhook. If that host's chain roots outside the common set, TLS stops verifying — and the failure appears in the field, on their device, at runtime, not in anyone's build. Fifty kilobytes of a partition that is 35% empty does not buy the right to that.

Worth revisiting only if flash becomes the constraint, and then with the trimmed set checked against every host the shipped Apps actually contact.

### The WiFi buffers: the size tool cannot see this lever at all

Both overrides verifiably landed in the generated `sdkconfig`, and the image was byte-identical to baseline. That is the answer: `CONFIG_ESP_WIFI_*_BUFFER_NUM` sizes pools allocated on the **heap** at `esp_wifi_init()`, so `idf.py size` — which reports static sections only — is structurally incapable of measuring them.

Every heap lever has this property. Measuring one means flashing the variant and reading `heap.free8Bit` from `GET /api/status`, which is a device round-trip per data point and cannot be done from a build. Candidates, in rough order of expected return:

- `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` / `DYNAMIC_TX_BUFFER_NUM` (32 each today, the arduino-esp32 values). The classic recommendation for RAM-tight builds, and the most likely real win.
- `ESP_WIFI_STATIC_RX_BUFFER_NUM` (8, also inherited). Named "static" but still heap-allocated by the driver.
- `LWIP_MAX_SOCKETS` (16 today vs the IDF default 10). Each socket carries control structures; the httpd, the DNS responder and one outbound TLS fetch can be live together, so 10 is probably fine but 16 is honest headroom.
- `LWIP_TCP_SND_BUF_DEFAULT` / `WND_DEFAULT` (5744/5760). Reducing these trades throughput for heap; the OTA download is the only throughput-sensitive path and it is not close to saturating the link.

**The cost of getting these wrong is not a smaller number.** Under-provisioned WiFi buffers present as packet loss under load — a stalled OTA, a dropped settings save, an intermittently unreachable device. That is worse than the heap it frees, and it will not reproduce on demand. If this is pursued, it wants one variable at a time, a soak with a large OTA in flight, and `heap.minFree` read after rather than during.

## A number that is not real: `libesp_stdio.a`

`idf.py size-components` attributes **98,974 bytes of Flash Data** to `libesp_stdio.a`, second only to `libmain.a`. It looked like the largest single saving available. It is an artefact.

The archive is 57,122 bytes on disk and contains three objects (`stdio_vfs.c.obj`, `stdio_simple.c.obj`, `stdio_syscalls_simple.c.obj`). `xtensa-esp32-elf-size -A` across all three shows a few hundred bytes of `.rodata` in total — `s_vfs_console` (52 B), `s_vfs_console_dir` (64 B), a handful of format strings. The per-object "Total" figures in that same output (19,352 / 1,787 / 6,854) include the Xtensa `.xt.prop` and `.xt.lit` link-relaxation metadata sections, which are not loaded into the image.

Treat the per-archive Flash Data column in `size-components` as indicative, not as a budget. When a row looks implausible for the amount of code in it, go to the archive.

## Recommendation

**Change nothing on flash.** The one real lever costs a consumer's TLS reliability to reclaim space in a partition that is a third empty.

**If heap is pursued**, do it as its own exercise with the device in the loop and one variable at a time — the build cannot answer the question, and the failure mode is intermittent rather than obvious. The migration bought ~47 KB of 8-bit headroom; there is no current symptom arguing for more.

The tuning that *did* pay was not a lever at all: it was moving off the precompiled Arduino libraries so `sdkconfig.defaults` became a file someone can read and change. Every line in it now says why it is there.
