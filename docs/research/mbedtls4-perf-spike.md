# mbedTLS 4 vs 3 TLS performance — claims, evidence, and a spike to test them

**Written:** 2026-08-24, as a spike plan. **Updated:** 2026-08-24, after running it on hardware. **Status: DONE. All twelve cells captured; C2 refuted, C3 confirmed. [§8](#8-results) is the answer — read it before §1–§6, which are the plan as written beforehand and are left unedited except where a claim they assert is now known to be false.**
**Why this exists:** during the IDF 6 migration I found that a TLS handshake cost 8.49 s where the arduino-esp32 build did the same request in 4.12 s, and I fixed it back to parity (4.21 s) by turning the RSA/MPI accelerator **off** and enabling fixed-point ECP. The *measurements* are solid. The *explanation* I gave for them was largely inference, and part of it is already falsified. This document separates the two and specifies an experiment that can settle it.

**Read this first if you only read one thing:** [§8](#8-results). The config matrix in §2 is trustworthy and reproducible; the causal story in §3 was not, and §8 replaces it with measurements. The accelerator is the wrong tool at ECC operand sizes in *both* mbedTLS versions — it is not a mbedTLS 4 problem, and the version gap is a separate, smaller effect.

**Picking this up with no context?** §0 is the handover and §8 is the answer. §1–§5 are the background, §6 the design, §7 what stands regardless.

---

## 0. State, and how to continue from nothing

**Where it stands.** Finished. All eight configurations plus four uninstrumented control cells have been flashed and captured; `spike/mbedtls-perf/results/` holds a `.log` per run and the collated `results.csv`. **C2 is refuted and C3 is confirmed** — [§8](#8-results) has the deciding numbers, and `sdkconfig.defaults` and [idf6-migration-continuation.md](idf6-migration-continuation.md) have been corrected where they asserted C2. The device was left running the spike firmware and needs the restore in *Flashing this spike destroys the device's firmware* below.

Everything from here to §7 is the plan as it was written before any measurement. It is left standing so the reasoning can be audited against the result — but where it asserts something the measurements have since falsified, that is marked inline.

### Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration`. Harness at `spike/mbedtls-perf/` — it has [its own README](../../spike/mbedtls-perf/README.md), which is the operational detail; this section is the orientation. |
| ESP-IDF 6.0.2 | `~/esp/esp-idf` → mbedTLS **4.1.0**. `& $HOME\esp\esp-idf\export.ps1` |
| ESP-IDF 5.5.5 | `~/esp/esp-idf-v5.5` → mbedTLS **3.6.6**. `& $HOME\esp\esp-idf-v5.5\export.ps1`. Installed for this spike; toolchain shares `~/.espressif` with 6.0.2's. |
| Device | ESP32-D0WD-V3, serial on **COM5** with RTS auto-reset wiring. |
| Sources to read | 3.6.6 at `~/esp/esp-idf-v5.5/components/mbedtls/mbedtls/`, 4.1.0 at `~/esp/esp-idf/components/mbedtls/mbedtls/` (crypto under `tf-psa-crypto/`). |

Only one SDK can be active per shell, and `export.ps1` is not idempotent across versions — use a fresh shell to switch rather than sourcing both.

### Flashing this spike destroys the device's firmware — read before the first flash

It writes its own bootloader and its own default `single_app` partition table, so **the smolbase firmware, the custom layout from `partitions.csv`, and the NVS holding the WiFi credentials all go.** This is expected and fine; it is also not reversible by re-flashing the app alone. Recovery, when the measurements are done:

```
& $HOME\esp\esp-idf\export.ps1
cd D:\source\smolbase
idf.py '@smolbase.args' -p COM5 -b 460800 flash    # bootloader + partitions + app + fs
```

The filesystem image rides along because `littlefs_create_partition_image` is declared `FLASH_IN_PROJECT`. The device then needs re-provisioning through the captive portal, by hand, on a phone.

`run.ps1` does nothing destructive without `-Flash`, which is why `-Flash` is not the default.

### What to actually type — to reproduce it

The runs below have all been done and their captures are committed. This is the recipe for repeating one, not a to-do list. Start with the highest-value cell — mbedTLS 3.6.6 with the accelerator **off**, the one §5.2 identifies as the single most valuable measurement in this document and the one the precompiled Arduino libraries made unreachable:

```
& $HOME\esp\esp-idf-v5.5\export.ps1
cd D:\source\smolbase\spike\mbedtls-perf
.\run.ps1 -Runs idf5-mpi-off -Flash
```

Then the other `idf5-*` in the same shell, and the `idf6-*` in a fresh shell with the 6.0.2 export. `.\run.ps1` with no `-Runs` covers all twelve cells, the four `-nowrap` controls included. `.\run.ps1 -ParseOnly` collates every capture in `results/` into `results/results.csv`. Rebuilding is only needed if the harness source changes — all twelve binaries are already on disk under `build/`.

### Before believing any number

- **`[ok] wrap active` must be in the capture.** A `-Wl,--wrap` that resolves nothing reports zero calls, which is indistinguishable from "this version never calls it".
- **`[vec] sig_r` / `sig_s` must be identical between the 3.6.6 and 4.1.0 captures.** RFC 6979 signing makes the signature a function of key and hash alone, so a difference means the two versions are not verifying the same thing and every `ecdsa_verify` row is incomparable.
- **`[id]` is the ground truth for what was measured** — `hw_mul` and `fixed_point` come from the compiled macros, not from the argfile name. `run.ps1` also asserts them in the generated `sdkconfig` before flashing, because a lever that failed to take reads as a perfectly plausible measurement of the wrong thing.
- **`[done]` must be present**, or the capture was truncated and its rows are partial.
- **`rc=0` on every row.** A primitive that errors out returns early and times as a suspiciously fast one.

### Then finish the job — done

§6.4 was the decision table and §6.6 the definition of done; [§8.7](#87-definition-of-done) checks the result against both. C2 did fall, so the correction it demanded has been made: neither `sdkconfig.defaults` nor the "Hard-won facts" section of [idf6-migration-continuation.md](idf6-migration-continuation.md) asserts it any more.

---

## 1. Versions and hardware under test

| | Arduino build (reference) | Native build (subject) |
|---|---|---|
| Firmware | smolbase v0.3.3 | smolbase 0.4.0-dev, branch `idf6-migration` |
| Framework | arduino-esp32 3.3.11 (pioarduino) | native ESP-IDF |
| ESP-IDF | 5.5.x (precompiled libs) | **6.0.2** |
| **mbedTLS** | **3.6.6** | **4.1.0** (TF-PSA-Crypto) |
| Chip | ESP32-D0WD-V3, 240 MHz, 8 MB flash, no PSRAM | same physical device |

Both mbedTLS source trees are **on disk** and can be diffed and built:

- 3.6.6 — `~/esp/esp-idf-v5.5/components/mbedtls/mbedtls/` (IDF 5.5.5), the **buildable** copy, installed for this spike. Also at `~/.platformio/packages/framework-espidf/components/mbedtls/mbedtls/`, which is the same 5.5.5 but readable only — PlatformIO ships the libraries precompiled, which is exactly why the accelerator-off cell for mbedTLS 3 was unmeasurable before (§5.2).
- 4.1.0 — `~/esp/esp-idf/components/mbedtls/mbedtls/` (IDF 6.0.2). The crypto is under `tf-psa-crypto/`; `bignum.c`, `ecp.c` and `ecdsa.c` are in `tf-psa-crypto/drivers/builtin/src/`, and their headers under `drivers/builtin/include/mbedtls/private/`.

The Arduino build's own generated IDF config is also on disk, which is what made the config elimination in §4 possible:
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig`

### What the handshake actually is

Captured with `CONFIG_MBEDTLS_DEBUG=y` + `MBEDTLS_DEBUG_LEVEL_DEBUG` on the native build:

- Negotiated suite: **`TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256`** (`c02b`), TLS 1.2
- Chain, **entirely ECDSA, no RSA anywhere**:
  - `CN=*.github.com` — EC key 256 bits, signed ECDSA-SHA256
  - `Sectigo Public Server Authentication CA DV E36` — EC 256, signed ECDSA-SHA384
  - `Sectigo Public Server Authentication Root E46` — EC 384, signed ECDSA-SHA384
- The ESP32 has **no ECC accelerator**: `MBEDTLS_HARDWARE_ECC` and `MBEDTLS_HARDWARE_ECDSA_VERIFY` both `depend on` SOC caps this chip lacks. EC maths is software on every IDF version.

---

## 2. The measured data (trustworthy, reproducible)

All timings are wall-clock of `GET /api/update/check` on the device, which performs exactly one HTTPS request to `api.github.com` and stream-parses ~30 KB of JSON. Harness: `scratchpad/tls_cost.ps1` — it opens the UART, times the request with a stopwatch, and counts `task_wdt` triggers in the same window. Watchdog timeout was the IDF default 5 s throughout.

### 2.1 The two-factor matrix

Run with the **buffered** reader (§2.3) in place, 4 reps each, on the native build:

| # | `MBEDTLS_HARDWARE_MPI` | `MBEDTLS_ECP_FIXED_POINT_OPTIM` | mean | reps | wdt trips | binary |
|---|---|---|---|---|---|---|
| A | on | off | 6.85 s | 7.25 / 6.66 / 6.80 / 6.70 | 4 | 1,457,232 |
| B | on | on | 6.50 s | 6.79 / 6.40 / 6.30 / 6.52 | 4 | 1,479,712 |
| C | off | off | 5.92 s | 6.18 / 5.76 / 5.86 / 5.88 | 4 | 1,451,632 |
| **D** | **off** | **on** | **4.25 s** | 4.48 / 4.10 / 4.25 / 4.17 | **0** | 1,474,080 |
| — | *Arduino v0.3.3 reference* | *(MPI on, FP off)* | *4.12 s* | *4.45 / 3.98 / 3.92* | *0* | *1,731,680* |

Confirmed separately after settling on D: 4.21 s mean (4.65 / 4.17 / 4.01 / 4.03), 0 trips.

**Two robust observations, independent of any explanation:**

1. Turning the accelerator **off** is worth ~0.9 s (A→C) and never costs anything.
2. The two factors **interact**: fixed-point ECP is worth 0.35 s with the accelerator on (A→B) but 1.67 s with it off (C→D). Measuring either alone understates the pair.

### 2.2 Rejected levers, also measured

| Lever | Condition | Result |
|---|---|---|
| `MBEDTLS_COMPILER_OPTIMIZATION_PERF` (mbedTLS at `-O2`) | MPI on, unbuffered reader | **9.38 s** (9.55 / 9.36 / 9.24) vs 8.49 s baseline — *worse*; +26 KB. Presumed flash-cache pressure, untested. |
| `MBEDTLS_ECP_FIXED_POINT_OPTIM` alone | MPI on, unbuffered reader | 7.93 s (8.08 / 7.75 / 7.96) vs 8.49 s — only 7%, which is why it was initially dismissed. |
| `MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` vs `_FULL` | — | −50,792 bytes flash, 0 DRAM, no timing effect measured. Not taken (consumer TLS risk). |

### 2.3 The one cause that is proven, and is ours

`core/Http`'s `ClientReader` was **unbuffered**: `read()` called `esp_http_client_read(_c, &ch, 1)` — one byte, through mbedTLS, per call — and ArduinoJson scans byte-by-byte. Instrumented phase split, native build, MPI on, unbuffered:

```
[spike] open(connect+tls)=5858  headers=378  body=2053  total=8291 ms
[spike] open(connect+tls)=5727  headers=25   body=2074  total=7827 ms
[spike] open(connect+tls)=5822  headers=28   body=2093  total=7944 ms
```

**~2.07 s of the 8.49 s was the body**, not the handshake. The Arduino build never had this cost because it passed `*http.getStreamPtr()` to `deserializeJson`, and that Stream is an arduino-esp32 `NetworkClient` owning a `NetworkClientRxBuffer` — a heap-allocated **1436-byte** (TCP MSS) buffer whose only purpose is making `Stream::read()` cheap (`NetworkClient.cpp:93`). Phase 4a of the migration replaced the Stream with a raw per-byte read and dropped it silently. Now restored at the same size, heap-allocated, same rationale.

This is settled and needs no spike. It is listed because **it means the headline 8.49 s vs 4.12 s comparison conflates two independent regressions**, and only one of them was ever about mbedTLS.

### 2.4 Stack samples during the slow handshake

Method: set `CONFIG_ESP_TASK_WDT_TIMEOUT_S=1` so the watchdog samples the stack repeatedly through one handshake; decode with `xtensa-esp32-elf-addr2line -pfiaC -e build/smolbase/smolbase.elf`. 8 samples captured (native build, MPI on, unbuffered):

| Samples | Where |
|---|---|
| 2 | `mbedtls_mpi_core_gcd_modinv_odd` (`bignum_core.c`), via `ecp_normalize_jac` and `mbedtls_ecdsa_verify_restartable` — with `mbedtls_ct_if` from `constant_time_impl.h` on the stack |
| 2 | `ecp_mul_restartable_internal` / `ecp_double_jac` → `mbedtls_mpi_mul_mod` |
| 3 | `touch_priv_execute_sw_filter` via `esp_timer` — **our** 200 Hz touch filter, running on core 0 |
| 1 | `multi_heap_malloc_impl` |

Two things to note. Half the samples are in EC/bignum, which is why I looked there. And **three of eight are our own touch filter** on core 0 — `SMOLBASE_TOUCH_FILTER_MS 5` schedules an `esp_timer` callback at 200 Hz, and `esp_timer` callbacks run pinned to core 0 alongside WiFi and TLS. That is an unquantified confound, not a finding (§5.4).

---

## 3. My claims, and their actual status

This is the part that needs the spike. I am labelling each claim honestly.

| # | Claim I made | Status |
|---|---|---|
| C1 | mbedTLS 4 moved modular inversion to a constant-time binary GCD with no accelerated path, and that is new hardening in 4.x | **FALSIFIED.** `gcd_modinv_odd` is present in mbedTLS **3.6.6** too — same files (`bignum.c`, `bignum_core.c`, `ecp.c`, `ecdsa.c`). Not new, not a 4.x change. |
| C2 | mbedTLS 4's ECP calls `mbedtls_mpi_mul_mpi` far more often than 3.x did, so the per-call hardware overhead multiplies out | **REFUTED, decisively ([§8.1](#81-c2-refuted--the-call-counts-are-identical)).** Both versions make **exactly 5817 calls per P-256 verify** (3912 with fixed-point ECP), and exactly 2905 per `ecp_mul` (1000 with it). Identical to the digit, in all four lever combinations. *Was, before the spike:* never counted, in either version, and static comparison already argued against it: `ecp.c`'s `mbedtls_mpi_mul_mod` helper is **character-identical** in 3.6.6 and 4.1.0, both routing through `mbedtls_mpi_mul_mpi`, with identical counts of `mul_mpi` (3), `mul_mod` (3), `MOD_MUL` (2), `sub_mod` (2), `add_mod` (2). The files differ by ~190 lines overall but not visibly on the multiply path. |
| C3 | Per-call hardware setup cost (lock acquire, clock enable, two hardware passes, clock disable, lock release) exceeds the software multiply at 256-bit operands | **CONFIRMED ([§8.2](#82-c3-confirmed--and-the-crossover-is-between-256-and-512-bits)).** At 256 bits the peripheral costs **19.49 µs against software's 9.78 µs** (3.6.6; 19.69 vs 11.28 in 4.1.0) — very close to 2× worse. The crossover is between 256 and 512 bits, and by 2048–4096 bits the peripheral is 3.7–4.0× *faster*. It is not version-specific: the per-call figures are within 1% between 3.6.6 and 4.1.0, as the byte-identical ESP port predicts. |
| C4 | The RSA accelerator can only hurt an ECDSA handshake because there is no RSA in it | **SUPPORTED** by the debug capture (§1) — the chain is all-ECDSA. Still an argument, not a measurement of the magnitude. |
| C5 | Turning the accelerator off is a net win; fixed-point ECP is a further win, and they interact | **MEASURED** (§2.1). |
| C6 | It is not a configuration difference and not the ESP port | **SUPPORTED** by direct elimination (§4). |
| C7 | #119 stays safe without `LARGE_KEY_SOFTWARE_MPI`, because with no hardware MPI there is no 4096-bit hardware limit to exceed | **VALIDATED** on hardware: a full self-update, which traverses GitHub's RSA-4096 ISRG Root X1 cross-signed CDN chain, completed in 41.8 s with 0 watchdog trips (was 52.5 s / 3 trips). |

**The uncomfortable summary, as written before the spike:** C5, C6 and C7 are established. C1 is wrong. C2 has no support and some contrary evidence. C3 — the only mechanism left standing — has never been measured. So I have a working fix whose *reason* I cannot currently defend.

**After the spike:** C1 and C2 are both wrong, C3 is right, and the fix's real reason is C3 alone — the accelerator is simply the wrong tool at ECC operand sizes, in *both* mbedTLS versions. The version gap is real but separate, smaller than claimed (23–34%, not 2×), and caused by mbedTLS 4 being slower per bignum operation at identical call counts. [§8](#8-results).

---

## 4. What has already been eliminated (do not re-do)

Direct comparison of the Arduino build's generated `sdkconfig` against ours. Every one of these is **identical**:

`MBEDTLS_HARDWARE_MPI` (y both) · `MBEDTLS_LARGE_KEY_SOFTWARE_MPI` (y both, at the time) · `MBEDTLS_MPI_USE_INTERRUPT` (absent both) · `MBEDTLS_ECP_NIST_OPTIM` (y both) · `MBEDTLS_ECP_FIXED_POINT_OPTIM` (n both) · full curve list incl. `CURVE25519` (both) · `MBEDTLS_SSL_PROTO_TLS1_3` (n both), TLS 1.2 only · `MBEDTLS_HARDWARE_SHA` / `_AES` (y both) · CPU 240 MHz · `COMPILER_OPTIMIZATION_SIZE` · no PSRAM · flash DIO @ 40 MHz.

Arduino additionally enabled *more* ciphersuites, curves and PSK modes than we do — which would make it slower, not faster.

The ESP bignum port is **byte-identical** between IDF 5.5 and 6.0.2 apart from one added `#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS`:

- `components/mbedtls/port/bignum/esp_bignum.c` — same `mbedtls_mpi_mul_mpi`, same `hw_words * 32 > SOC_RSA_MAX_BIT_LEN/2` gating, no lower threshold in either
- `components/mbedtls/port/bignum/bignum_alt.c` — same `esp_mpi_enable_hardware_hw_op()` (lock acquire → clock enable → hal enable) and `disable`

DNS is eliminated: reps 2–4 would have hit a warm resolver cache and were within 0.3 s of rep 1 every time.

---

## 5. Confounds in the existing data — read before trusting any of it

These are the reasons the current numbers cannot settle the mechanism, and the spike must avoid them.

**5.1 The 8.49 vs 4.12 comparison is not apples-to-apples.** It differs by mbedTLS version *and* HTTP client stack *and* JSON reader buffering *and* application code. §2.3 showed 2.07 s of it was the reader alone. Any residual attribution to mbedTLS is contaminated.

**5.2 The critical control was never run: mbedTLS 3 with the accelerator OFF.** Every Arduino measurement had `HARDWARE_MPI=y`, because the libraries are precompiled and cannot be reconfigured. So the matrix has a hole:

| | MPI on | MPI off |
|---|---|---|
| mbedTLS 3.6.6 | 4.12 s (measured) | **never measured** |
| mbedTLS 4.1.0 | 6.85 s (measured) | 5.92 s (measured) |

If mbedTLS 3 is *also* substantially faster with the accelerator off, then the accelerator is simply wrong for ECDSA on this chip in both versions, and it explains **none** of the version gap. That single missing cell is the highest-value measurement in this document.

> **It was measured, and that is what it says.** mbedTLS 3.6.6 is 17.8% faster with the accelerator off — a larger effect than 4.1.0's 10.7%. [§8.3](#83-the-missing-cell-52--the-accelerator-was-always-wrong).

**5.3 Wall-clock over WiFi to a live internet host** includes network variance, GitHub's server behaviour, and possible TLS session-ticket effects. Reps were tight (±0.3 s) which bounds the noise, but the Arduino and native measurements were taken hours apart against a third-party server.

**5.4 Our own touch filter runs on core 0 at 200 Hz** (3 of 8 stack samples). It is present in both builds, so it does not explain the delta, but it inflates every absolute number and adds jitter.

**5.5 Timings are of a whole request, not of the crypto.** Even the phase split only isolates `esp_http_client_open`, which bundles TCP connect with the handshake.

**5.6 The `-O2`-is-worse result has no mechanism.** Flash-cache pressure is a guess.

---

## 6. The spike

**Goal:** determine, offline and per-operation, why mbedTLS 4.1.0 is slower than 3.6.6 on this workload, and whether the accelerator's per-call overhead (C3) is the mechanism.

**Design principle:** no network, no HTTP, no application. Fixed test vectors, N iterations, `esp_timer` timing, results over UART. Everything that made §2 contaminated is removed.

### 6.1 Harness

**Built, green on both SDKs, and now captured.** `spike/mbedtls-perf/` exists, with its own [README](../../spike/mbedtls-perf/README.md); all eight configurations compile warning-clean and their generated `sdkconfig`s have been asserted. All eight have since been flashed and captured, along with four uninstrumented control cells added during the run ([§8.6](#86-a-harness-defect-found-by-the-spike-and-the-control-that-caught-it)).

A standalone project that builds **unchanged** under both IDF 5.5.5 (mbedTLS 3.6.6) and IDF 6.0.2 (4.1.0) — whichever `export.ps1` is sourced picks the version.

**This paragraph originally claimed the five primitives were public API in both versions, so one source file would compile against each. That is wrong.** In mbedTLS 4.1.0 the crypto moved into TF-PSA-Crypto and `bignum.h`, `ecp.h` and `ecdsa.h` are all under `mbedtls/private/`, reachable only with `MBEDTLS_ALLOW_PRIVATE_ACCESS` defined. The symbols themselves are unchanged and still exported, so it is purely an include-path and visibility problem, absorbed by `main/compat.h` — which discriminates on `MBEDTLS_VERSION_MAJOR` from `build_info.h`, *not* on IDF's `MBEDTLS_MAJOR_VERSION` compile definition, because IDF 6 exports that and IDF 5.5 does not. Hashing was dropped from the harness entirely (a fixed 32-byte array stands in for the digest) rather than write a second shim for the one API that genuinely changed.

Benchmarks, each timed over enough iterations to swamp `esp_timer` granularity:

1. `mbedtls_ecdsa_verify` on secp256r1 with a fixed key/hash/signature vector — the operation the handshake actually spends its time on.
2. `mbedtls_ecp_mul` on secp256r1 with a fixed scalar — isolates point multiplication.
3. `mbedtls_mpi_mul_mpi` on fixed operands at **256, 512, 1024, 2048 and 4096 bits** — **the per-call cost across sizes**, which is C3's crux. The plan asked only for 256 bits; the sweep is the improvement, because if C3 holds there is a crossover size above which the peripheral genuinely wins, and the ESP port applies no lower threshold. One number at 256 bits says the accelerator loses there; the curve says where it starts to pay, which is the difference between a measurement and an explanation.
4. `mbedtls_mpi_exp_mod` with 2048-bit and 4096-bit operands, `e = 65537` — an RSA *verify*, which is what a certificate chain actually does and what #119 was about. Confirms the accelerator still earns its place for RSA, and quantifies what turning it off costs there.
5. `mbedtls_mpi_inv_mod` on a 256-bit modulus — tests the inversion path (C1's remnant) in both versions.
6. `mpi_mul_hooked`: the same 256-bit multiply through the `--wrap` hook. Its margin over benchmark 3 is the instrumentation's own cost, which is what licenses reading `mul_ns_per_op` as a real number rather than an artefact. Every other benchmark calls the unwrapped symbol directly.

   > **This is wrong, and the spike caught it.** That margin is not the hook's cost — a build with no hook linked reproduces it. It comes from benchmark ordering, because `mpi_mul_hooked` shares a destination MPI that the preceding 4096-bit sweep has left oversized. The hook's real cost is ≤1.30 µs per call, established by the `SPIKE_NO_WRAP` control instead. [§8.6](#86-a-harness-defect-found-by-the-spike-and-the-control-that-caught-it).

The benchmark task is pinned to **core 1**, and confound 5.4 is gone by construction rather than by stopping anything — this app has no touch filter, no WiFi and no display. Batch sizes are calibrated at run time so each benchmark is timed over ~200 ms regardless of how expensive one operation is, and min/mean/max over five batches is reported so a scheduler hiccup is visible instead of averaged in.

Two deliberate departures from the plan's sketch, both in §6.1's favour: the hooks are not `volatile` (one writer, a pinned task, nothing else in the app touches mbedTLS), and there is no hashing at all — a fixed 32-byte array stands in for the digest, since ECDSA verify does not care and it avoids a second compat shim for the one API that genuinely changed.

### 6.2 Counting calls without patching IDF

`mbedtls_mpi_mul_mpi` and `mbedtls_mpi_exp_mod` are global symbols the ESP port overrides, so they can be counted non-invasively with linker wrapping:

The planned form — `target_link_options(${COMPONENT_LIB} INTERFACE ...)` — was replaced by a build property, because it has to reach the final executable link on both IDF 5.5 and 6.0. **It must come before `project()`**; after it the option is accepted in silence and the link then fails on `__real_mbedtls_mpi_mul_mpi`. What `spike/mbedtls-perf/CMakeLists.txt` actually does:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=mbedtls_mpi_mul_mpi" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=mbedtls_mpi_exp_mod" APPEND)

project(mbedtls_perf)
```

```c
static volatile uint32_t g_mul_calls, g_exp_calls;
static volatile int64_t  g_mul_us;
int __real_mbedtls_mpi_mul_mpi(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y);
int __wrap_mbedtls_mpi_mul_mpi(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y)
{
    const int64_t t0 = esp_timer_get_time();
    const int r = __real_mbedtls_mpi_mul_mpi(Z, X, Y);
    g_mul_us += esp_timer_get_time() - t0;
    g_mul_calls++;
    return r;
}
```

This yields, per ECDSA verify and per version: **call count**, **total time in the hooked function**, and by subtraction the time outside it. That is a direct test of C2 (counts) and C3 (per-call cost) in one shot. Verify the wrap actually took — a `--wrap` that silently fails to resolve looks exactly like "zero calls".

Note `ecp.c` has an internal `mul_count` behind `INC_MUL_COUNT`, but it is `static` and gated on `MBEDTLS_SELF_TEST`, so it is not readable from application code. Use the wrapper.

### 6.3 Runs

The plan called for four builds and a conditional repeat with `ECP_FIXED_POINT_OPTIM=y`. All eight exist instead: the two levers interact (§2.1), and a cold build per cell is cheaper than deciding later that the crossed cells were needed after all. **Built: all eight, plus four `-nowrap` controls. Captured: all twelve ([§8](#8-results)).**

| Argfile | IDF / mbedTLS | `HARDWARE_MPI` | `FIXED_POINT` | Purpose |
|---|---|---|---|---|
| `idf6-mpi-on.args` | 6.0.2 / 4.1.0 | on | off | subject, as originally shipped |
| `idf6-mpi-off-fp.args` | 6.0.2 / 4.1.0 | off | on | subject, as now shipped |
| `idf5-mpi-on.args` | 5.5.5 / 3.6.6 | on | off | reference, matching Arduino |
| **`idf5-mpi-off.args`** | **5.5.5 / 3.6.6** | **off** | **off** | **the missing cell (§5.2) — run this one first** |
| `idf6-mpi-off.args` · `idf6-mpi-on-fp.args` | 6.0.2 / 4.1.0 | | | the remaining v4 corners |
| `idf5-mpi-off-fp.args` · `idf5-mpi-on-fp.args` | 5.5.5 / 3.6.6 | | | the remaining v3 corners |

Each argfile carries its own `-B build/<tag>` and its own `SDKCONFIG=build/<tag>/sdkconfig`, so no two configurations can ever share a generated config.

Binary sizes, as a sanity check that the levers did something: v3 189,152 (off/off) → 224,960 (on/on); v4 170,480 → 194,544. Fixed-point ECP is the ~23–35 KB of precomputation tables; the accelerator is a few hundred bytes either way.

### 6.4 What each outcome would mean

| Observation | Conclusion |
|---|---|
| v4 makes materially more `mul_mpi` calls per verify than v3 | **C2 confirmed** — the version gap is call frequency. |
| v3 and v4 make the same number of calls | **C2 refuted.** The gap is per-call cost or work outside `mul_mpi`; look at the time-outside-hook figure and at `inv_mod`. |
| Hardware per-call cost at 256-bit exceeds software | **C3 confirmed** — the accelerator is wrong for ECC-sized operands, independent of version. |
| Hardware is faster per call at 256-bit | **C3 refuted**, and the matrix in §2.1 needs a different explanation entirely. |
| Run 4 ≈ Run 3 (v3 indifferent to the accelerator) | The accelerator interacts badly with **v4 specifically** — a real version-dependent finding. |
| Run 4 ≪ Run 3 (v3 also much faster with it off) | **The accelerator was always wrong for this workload**, in both versions, and explains none of the version gap. My fix is still correct; my reason for it is not. |
| v4 verify time ≈ v3 verify time offline | The version gap is **not** in the crypto at all, and §2's remaining delta is elsewhere — client stack, chain handling, or measurement error. |

### 6.5 Practical notes

- **IDF 5.5 is installed**, at `~/esp/esp-idf-v5.5` (tag `v5.5.5`, matching the PlatformIO package's 5.5.5 and therefore its mbedTLS 3.6.6), with its toolchain in the shared `~/.espressif` alongside 6.0.2's. Activate with `& $HOME\esp\esp-idf-v5.5\export.ps1`. The PlatformIO copy remains the source of truth for *reading* 3.6.6, not for building.
- **Each configuration gets its own build directory and its own generated `sdkconfig`** (the eight `idf{5,6}-*.args` argfiles), which is what makes the `sdkconfig.defaults`-cannot-override-`sdkconfig` trap below structurally impossible rather than merely documented.
- **`-Wl,--wrap` must be set before `project()`.** `idf_build_set_property(LINK_OPTIONS ...)` after it is accepted silently and then fails at link with `undefined reference to __real_mbedtls_mpi_mul_mpi`. That is the good failure; the bad one is a `--wrap` that resolves nothing and reports zero calls, which `bench.c` checks for at run time before any count is trusted.
- **`CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_*` exists only in IDF 6**, where it defaults to SIZE. IDF 5.5 has no per-component override and mbedTLS inherits the global level. Both SDKs land on `-Os`, by different routes; naming the IDF 6 symbol in a shared `sdkconfig.defaults` makes IDF 5.5 warn about an unknown Kconfig symbol.
- **Serial is on COM5** with RTS auto-reset. Read it with `ReadBufferSize >= 262144` and a tight `ReadExisting()` loop — a default `SerialPort` drops bytes during a burst and produces plausible-but-wrong text.
- **Editing any of the three `sdkconfig.*` files means deleting the affected `build/<tag>/sdkconfig`** before rebuilding; defaults cannot override an existing generated config. That is the one trap the per-cell build directories do *not* protect against, because it is about editing a fragment after a cell has already been configured.
- **A Kconfig `choice` needs the current pick explicitly unset** (`# CONFIG_X_SIZE is not set` *then* `CONFIG_X_PERF=y`). Neither lever in this spike is a choice, so it does not bite here — but `MBEDTLS_COMPILER_OPTIMIZATION` is one, and reaching for `-O2` (§2.2) would meet it.
- **`run.ps1` is the driver** — flash, capture, parse, collate. The earlier `tls_cost.ps1` and `perm_matrix.ps1` from the migration sessions were scratch files, never in the repo, and are gone; nothing here depends on them.

### 6.6 Definition of done

The spike is finished when there is a table of per-operation timings and call counts for all four runs, and each of C2 and C3 is marked confirmed or refuted **with the number that decided it**. If the conclusion is "the accelerator was always wrong and the version gap is elsewhere", that is a successful spike — it corrects the record. A spike that merely reproduces the wall-clock numbers from §2 has not done its job.

Concretely, done means: `spike/mbedtls-perf/results/` holds a `<tag>.log` per captured run and the collated `results.csv`; §3's status table has C2 and C3 resolved with their deciding numbers; and if C2 falls, `sdkconfig.defaults` and [idf6-migration-continuation.md](idf6-migration-continuation.md) no longer assert it. Commit the captures — they are the evidence, and a table without them is a claim.

The harness reports one `[bench]` line per benchmark, with `min_ns`/`mean_ns`/`max_ns`, `mul_calls_per_op` and `mul_ns_per_op` (both scaled ×1000, integers because newlib-nano's `printf` drops `%f`), plus `mpi_mul_hooked` whose margin over `mpi_mul`/256 prices the instrumentation itself. `--wrap` only sees cross-translation-unit calls, so multiplies made from inside the defining TU are invisible; `ecp.c` is a separate TU, so the path the stack samples implicated is counted.

---

## 7. Independent of the outcome

These are already committed and stand regardless of what the spike finds:

- The buffered `ClientReader` (§2.3) — a proven regression of ours, fixed.
- The config change to `HARDWARE_MPI` off + `FIXED_POINT_OPTIM` on — justified by §2.1's end-to-end measurements and validated by a full self-update, whatever the mechanism turns out to be.
- `LARGE_KEY_SOFTWARE_MPI`'s removal, and the reasoning in `sdkconfig.defaults` for why #119 stays safe.

---

## 8. Results

**Captured 2026-08-24 on the bench device over COM5.** Twelve cells: the eight planned, plus four uninstrumented controls added mid-spike (§8.6). Every capture passed the checks in §0 — `[ok] wrap active` present, `[done]` present, `rc=0` on every row, `[id]` matching its argfile, and `sig_r`/`sig_s` **identical across 3.6.6 and 4.1.0**, so the verify rows compare the same work. Raw captures are in `spike/mbedtls-perf/results/*.log` and the collation in `results.csv`.

Headline: **C2 is refuted, C3 is confirmed, and the version gap is real but a third the size I claimed.** The fix shipped in `sdkconfig.defaults` is correct; the reason recorded next to it was not, and has been corrected.

### 8.1 C2 refuted — the call counts are identical

`mul_calls_per_op` on `ecdsa_verify_p256`, and on `ecp_mul_p256`:

| | 3.6.6 | 4.1.0 |
|---|---|---|
| `ecdsa_verify_p256`, fixed-point off | **5817** | **5817** |
| `ecdsa_verify_p256`, fixed-point on | **3912** | **3912** |
| `ecp_mul_p256`, fixed-point off | **2905** | **2905** |
| `ecp_mul_p256`, fixed-point on | **1000** | **1000** |

Identical to the digit, in every lever combination, on both SDKs. mbedTLS 4.1.0 does **not** call `mbedtls_mpi_mul_mpi` more often than 3.6.6 — it calls it exactly as often. This is the outcome §6.4 labelled *"C2 refuted. The gap is per-call cost or work outside `mul_mpi`."* It also matches what §3 already suspected from the character-identical `mbedtls_mpi_mul_mod` helper: the static evidence was right and my claim was wrong.

The counts are also the cleanest thing in the whole spike — a count cannot be distorted by timing overhead, scheduling, or cache behaviour.

**Side benefit: this is the mechanism of the fixed-point win.** `ECP_FIXED_POINT_OPTIM` works by cutting calls (5817 → 3912 per verify, −32.8%; 2905 → 1000 per `ecp_mul`, −65.6%), whereas turning the accelerator off works by cutting cost per call. Two independent levers acting on the two factors of the same product — which is exactly why §8.5 finds them independent rather than interacting.

### 8.2 C3 confirmed — and the crossover is between 256 and 512 bits

`mpi_mul`, mean µs per call, measured through the *unhooked* path so no instrumentation is involved:

| operand bits | 3.6.6 software | 3.6.6 accelerator | 4.1.0 software | 4.1.0 accelerator |
|---|---|---|---|---|
| **256** | **9.78** | **19.49** | **11.28** | **19.69** |
| 512 | 29.22 | 22.15 | 32.05 | 22.29 |
| 1024 | 100.80 | 36.04 | 106.56 | 36.18 |
| 2048 | 378.74 | 76.51 | 391.29 | 76.65 |
| 4096 | 1467.20 | 457.92 | 1496.11 | 462.30 |

**At 256 bits the accelerator costs almost exactly twice a software multiply.** C3 confirmed, with the number that decides it: 19.49 µs against 9.78 µs.

The shape is the giveaway and is worth more than the single cell. The software column scales roughly with the square of the operand size (9.8 → 1467, a factor of 150 across 16× the bits). The accelerator column is nearly **flat** from 256 to 1024 bits (19.5 → 22.2 → 36.0) — that flatness *is* the fixed per-call ceremony, dominating everything at small sizes, exactly as the port's lack of a lower size threshold predicts. The crossover falls between 256 and 512 bits, and P-256 sits on the losing side of it.

**And the accelerator emphatically earns its place above the crossover** — `mpi_exp_mod` with `e = 65537`, mean ms:

| | 3.6.6 software | 3.6.6 accelerator | 4.1.0 software | 4.1.0 accelerator |
|---|---|---|---|---|
| 2048-bit | 61.77 | 17.03 (**3.6×**) | 74.50 | 19.98 (**3.7×**) |
| 4096-bit | 227.03 | 54.56 (**4.2×**) | 262.85 | 65.67 (**4.0×**) |

This prices what the shipped config gives up. Turning `HARDWARE_MPI` off costs **+197 ms per RSA-4096 verify** (65.67 → 262.85 ms on 4.1.0) — real, and paid on GitHub's ISRG Root X1 cross-signed CDN chain in #119. It is affordable because a chain does one or two such verifies while an ECDSA handshake does thousands of 256-bit multiplies, which is why the self-update got *faster* overall (52.5 s → 41.8 s) despite this.

The per-call accelerator figures are within 1% between the two mbedTLS versions (19.49 vs 19.69 µs), as the byte-identical ESP bignum port established in §4 predicts. **C3 is a property of the chip and the port, not of mbedTLS 4.**

### 8.3 The missing cell (§5.2) — the accelerator was always wrong

From the uninstrumented control builds, `ecdsa_verify_p256` mean ms, fixed-point off:

| | accelerator on | accelerator off | gain from turning it off |
|---|---|---|---|
| **mbedTLS 3.6.6** | 298.22 | **245.12** | **−17.8%** |
| **mbedTLS 4.1.0** | 366.86 | 327.75 | −10.7% |

`ecp_mul_p256` agrees: 139.68 → 113.92 (−18.4%) on 3.6.6, 170.46 → 151.56 (−11.1%) on 4.1.0.

This is §6.4's *"Run 4 ≪ Run 3"* row: **the accelerator was always wrong for this workload, in both versions, and explains none of the version gap.** If anything the effect is *larger* in 3.6.6 than in 4.1.0. The cell the precompiled Arduino libraries made unreachable is filled, and it says the thing that was least convenient for my original story.

**My fix is still correct; my reason for it was not.** The correct reason is C3 alone, and it would have applied equally to the Arduino build had it been reconfigurable.

### 8.4 The version gap is real, separate, and about a third of what I claimed

Same control builds, like-for-like:

| | 3.6.6 | 4.1.0 | 4.1.0 slower by |
|---|---|---|---|
| `ecdsa_verify_p256`, accelerator on | 298.22 ms | 366.86 ms | **+23.0%** |
| `ecdsa_verify_p256`, accelerator off | 245.12 ms | 327.75 ms | **+33.7%** |
| `ecp_mul_p256`, accelerator on | 139.68 ms | 170.46 ms | +22.0% |
| `ecp_mul_p256`, accelerator off | 113.92 ms | 151.56 ms | +33.0% |

**23–34%, not the "roughly 2×" I wrote into the continuation note.** That 2× came from the 8.49 s vs 4.12 s end-to-end comparison, of which §2.3 had already shown ~2.07 s was our own unbuffered reader — §5.1's warning, vindicated.

With call counts identical (§8.1), the gap has to be per-operation cost, and it is. Software-path primitives, 3.6.6 → 4.1.0:

| primitive | 3.6.6 | 4.1.0 | change |
|---|---|---|---|
| `mpi_inv_mod` 256-bit | 8.664 ms | 12.231 ms | **+41.2%** |
| `mpi_exp_mod` 2048-bit | 61.77 ms | 74.50 ms | +20.6% |
| `mpi_exp_mod` 4096-bit | 227.03 ms | 262.85 ms | +15.8% |
| `mpi_mul` 256-bit | 9.78 µs | 11.28 µs | +15.3% |
| `mpi_mul` 512-bit | 29.22 µs | 32.05 µs | +9.7% |
| `mpi_mul` 1024-bit | 100.80 µs | 106.56 µs | +5.7% |
| `mpi_mul` 2048-bit | 378.74 µs | 391.29 µs | +3.3% |
| `mpi_mul` 4096-bit | 1467.20 µs | 1496.11 µs | +2.0% |

Two things fall out. **`mpi_inv_mod` is the worst single regression at +41%** — which is where C1 was looking, and it is worth noting that C1 was wrong about *why* (the constant-time binary GCD is present in 3.6.6 too, §3) while still having pointed at a genuinely slower path.

And the `mpi_mul` regression **shrinks monotonically as operands grow**, +15.3% at 256 bits down to +2.0% at 4096. That is the signature of added *per-call* cost — entry validation, parameter marshalling, the TF-PSA-Crypto indirection — rather than slower inner loops, since a fixed overhead is a large fraction of a 9.8 µs multiply and a negligible one of a 1467 µs multiply. **This is a hypothesis consistent with the shape, not a measurement**; isolating it would mean profiling inside `mbedtls_mpi_mul_mpi`, which this spike does not do. I am flagging it rather than repeating the C2 mistake of promoting a plausible mechanism to a found one.

### 8.5 The two levers are independent, and §2.1's interaction is probably an artefact

§2.1 reported a strong interaction: fixed-point ECP worth 0.35 s with the accelerator on but 1.67 s with it off. Offline, the primitives say the two levers are **independent multiplicative factors**:

| | 3.6.6 | 4.1.0 |
|---|---|---|
| accelerator off, as a fraction of baseline | ×0.841 | ×0.894 |
| fixed-point on, as a fraction of baseline | ×0.694 | ×0.695 |
| predicted if independent | ×0.584 | ×0.621 |
| **measured, both applied** | **×0.592** | **×0.625** |

Within 1.4% and 0.6% respectively. §8.1 explains why: one lever cuts the number of multiplies, the other cuts the cost of each, and the product is just the product.

So §2.1's interaction most likely is not a property of the crypto. The available explanation is that cells A, B and C **tripped the task watchdog on every request** and cell D did not — a wall-clock measurement that includes watchdog handling is not measuring the same thing as one that does not. That is an explanation offered, not established; the end-to-end numbers themselves are unaffected and the config choice they justified stands either way.

### 8.6 A harness defect found by the spike, and the control that caught it

Mid-spike, `mpi_mul_hooked` appeared to show the instrumentation costing **2.33 µs per call with the accelerator off but 11.08 µs with it on** — reproducibly, and identically in both mbedTLS versions. Since `ecdsa_verify` makes 5817 hooked calls, that would have put ~64 ms of instrumentation in every accelerator-on verify row against ~14 ms in the accelerator-off rows: an artefact the same order as the §8.3 effect, pointing the same direction, and capable of manufacturing the entire result.

So four uninstrumented control cells were built (`-D SPIKE_NO_WRAP=1`, which omits the `-Wl,--wrap` link options and compiles out the hooks) and flashed. They settle it in both directions:

- **The hook's real cost is small**: same cell, wrapped minus no-wrap, over 5817 calls — **0.21 to 1.30 µs per call**, ≤2.3% of a verify. Every wrapped measurement in §8.1–§8.5 stands.
- **`mpi_mul_hooked` is not measuring the hook.** In a build with no hook linked at all, its margin over `mpi_mul` is still 9.77 µs with the accelerator on and 1.20 µs with it off — the same margin, with nothing to instrument.

What the margin actually is: `mpi_mul_hooked` runs *after* the 256→4096 sweep and shares the destination MPI `g_mul_x`, which the 4096-bit multiply has by then grown to 8192 bits. A 256-bit multiply into an oversized destination is not the same operation as one into a right-sized destination, and the accelerator path is far more sensitive to that than the software path. **That is the most likely explanation and it is not confirmed** — confirming it needs a reordered or destination-resetting build. Either way it is now a documented harness limitation: **`mpi_mul_hooked` does not price the instrumentation, and §6.1's claim that it does is wrong.** The no-wrap control does, and is the right tool. Fixing the ordering is a small follow-up, and nothing in §8 depends on it.

This is the §6.6 principle applied to the spike's own instrument: the number that would have been most flattering to my original story was the one produced by a measurement error, and only a control that removed the instrument entirely could tell the difference.

### 8.7 Definition of done

Against §6.6:

- ✅ Per-operation timings and call counts for all four planned runs — twelve cells captured, not four.
- ✅ C2 marked **refuted**, deciding number: 5817 calls per verify in *both* versions (§8.1).
- ✅ C3 marked **confirmed**, deciding number: 19.49 µs hardware vs 9.78 µs software at 256 bits (§8.2).
- ✅ Captures committed — `spike/mbedtls-perf/results/*.log` and `results.csv`.
- ✅ C2 no longer asserted in `sdkconfig.defaults` or [idf6-migration-continuation.md](idf6-migration-continuation.md); both now carry the measured mechanism instead, and the continuation note's "roughly 2× slower" is corrected to 23–34%.

The conclusion is the one §6.6 explicitly named as a successful outcome: *"the accelerator was always wrong and the version gap is elsewhere."* It corrects the record.

### 8.8 What is still open

- **The `mpi_mul_hooked` ordering defect** (§8.6) — mechanism unconfirmed, fix is a small harness change.
- **Where mbedTLS 4's per-call overhead lives** (§8.4) — the monotonic shrink with operand size implicates per-call cost, but nothing was measured inside `mbedtls_mpi_mul_mpi`.
- **`mpi_inv_mod`'s +41%** (§8.4) — the largest single regression, unexplained. C1 died on the question of whether the code was *new*; nobody has asked why the same algorithm is 41% slower.
- **The upstream report** — now worth filing with the corrected figure (23–34% per P-256 verify at identical call counts, plus the separate finding that the ESP RSA accelerator is a ~2× loss at ECC operand sizes in every version). Still search before asserting anything is undiscovered.

What the spike can change is the *explanation* recorded in `sdkconfig.defaults` and [idf6-migration-continuation.md](idf6-migration-continuation.md), both of which currently assert C2 as though it were established. **If the spike refutes C2, those comments must be corrected.**
