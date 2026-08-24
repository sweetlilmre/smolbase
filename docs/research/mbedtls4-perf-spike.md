# mbedTLS 4 vs 3 TLS performance — claims, evidence, and a spike to test them

**Written:** 2026-08-24, to be executed as a spike.
**Why this exists:** during the IDF 6 migration I found that a TLS handshake cost 8.49 s where the arduino-esp32 build did the same request in 4.12 s, and I fixed it back to parity (4.21 s) by turning the RSA/MPI accelerator **off** and enabling fixed-point ECP. The *measurements* are solid. The *explanation* I gave for them was largely inference, and part of it is already falsified. This document separates the two and specifies an experiment that can settle it.

**Read this first if you only read one thing:** the config matrix in §2 is trustworthy and reproducible. The causal story in §3 is not established. Do not repeat my mistake of treating a plausible mechanism as a found one.

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

- 3.6.6 — `~/.platformio/packages/framework-espidf/components/mbedtls/mbedtls/` (IDF 5.5)
- 4.1.0 — `~/esp/esp-idf/components/mbedtls/mbedtls/` (IDF 6.0.2)

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
| C2 | mbedTLS 4's ECP calls `mbedtls_mpi_mul_mpi` far more often than 3.x did, so the per-call hardware overhead multiplies out | **UNSUPPORTED.** Never counted, in either version. Worse, static comparison argues *against* it: `ecp.c`'s `mbedtls_mpi_mul_mod` helper is **character-identical** in 3.6.6 and 4.1.0, both routing through `mbedtls_mpi_mul_mpi`, with identical counts of `mul_mpi` (3), `mul_mod` (3), `MOD_MUL` (2), `sub_mod` (2), `add_mod` (2). The files differ by ~190 lines overall but not visibly on the multiply path. |
| C3 | Per-call hardware setup cost (lock acquire, clock enable, two hardware passes, clock disable, lock release) exceeds the software multiply at 256-bit operands | **UNTESTED, and the most plausible surviving mechanism.** The code path is real and verified by reading `esp_bignum.c` / `bignum_alt.c`: there is **no lower operand-size threshold**, so a 256×256-bit multiply pays the full peripheral ceremony, and on ESP32 a mod-mult needs *two* hardware passes. But the per-op cost was never measured against software. |
| C4 | The RSA accelerator can only hurt an ECDSA handshake because there is no RSA in it | **SUPPORTED** by the debug capture (§1) — the chain is all-ECDSA. Still an argument, not a measurement of the magnitude. |
| C5 | Turning the accelerator off is a net win; fixed-point ECP is a further win, and they interact | **MEASURED** (§2.1). |
| C6 | It is not a configuration difference and not the ESP port | **SUPPORTED** by direct elimination (§4). |
| C7 | #119 stays safe without `LARGE_KEY_SOFTWARE_MPI`, because with no hardware MPI there is no 4096-bit hardware limit to exceed | **VALIDATED** on hardware: a full self-update, which traverses GitHub's RSA-4096 ISRG Root X1 cross-signed CDN chain, completed in 41.8 s with 0 watchdog trips (was 52.5 s / 3 trips). |

**The uncomfortable summary:** C5, C6 and C7 are established. C1 is wrong. C2 has no support and some contrary evidence. C3 — the only mechanism left standing — has never been measured. So I have a working fix whose *reason* I cannot currently defend.

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

**5.3 Wall-clock over WiFi to a live internet host** includes network variance, GitHub's server behaviour, and possible TLS session-ticket effects. Reps were tight (±0.3 s) which bounds the noise, but the Arduino and native measurements were taken hours apart against a third-party server.

**5.4 Our own touch filter runs on core 0 at 200 Hz** (3 of 8 stack samples). It is present in both builds, so it does not explain the delta, but it inflates every absolute number and adds jitter.

**5.5 Timings are of a whole request, not of the crypto.** Even the phase split only isolates `esp_http_client_open`, which bundles TCP connect with the handshake.

**5.6 The `-O2`-is-worse result has no mechanism.** Flash-cache pressure is a guess.

---

## 6. The spike

**Goal:** determine, offline and per-operation, why mbedTLS 4.1.0 is slower than 3.6.6 on this workload, and whether the accelerator's per-call overhead (C3) is the mechanism.

**Design principle:** no network, no HTTP, no application. Fixed test vectors, N iterations, `esp_timer` timing, results over UART. Everything that made §2 contaminated is removed.

### 6.1 Harness

**Built, and green on both SDKs.** `spike/mbedtls-perf/` exists, with its own [README](../../spike/mbedtls-perf/README.md); all eight configurations compile warning-clean and their generated `sdkconfig`s have been asserted. Nothing has been flashed or measured yet — that step erases the smolbase firmware and is the operator's call.

A standalone project that builds **unchanged** under both IDF 5.5.5 (mbedTLS 3.6.6) and IDF 6.0.2 (4.1.0) — whichever `export.ps1` is sourced picks the version.

**This paragraph originally claimed the five primitives were public API in both versions, so one source file would compile against each. That is wrong.** In mbedTLS 4.1.0 the crypto moved into TF-PSA-Crypto and `bignum.h`, `ecp.h` and `ecdsa.h` are all under `mbedtls/private/`, reachable only with `MBEDTLS_ALLOW_PRIVATE_ACCESS` defined. The symbols themselves are unchanged and still exported, so it is purely an include-path and visibility problem, absorbed by `main/compat.h` — which discriminates on `MBEDTLS_VERSION_MAJOR` from `build_info.h`, *not* on IDF's `MBEDTLS_MAJOR_VERSION` compile definition, because IDF 6 exports that and IDF 5.5 does not. Hashing was dropped from the harness entirely (a fixed 32-byte array stands in for the digest) rather than write a second shim for the one API that genuinely changed.

Benchmarks, each timed over enough iterations to swamp `esp_timer` granularity:

1. `mbedtls_ecdsa_verify` on secp256r1 with a fixed key/hash/signature vector — the operation the handshake actually spends its time on.
2. `mbedtls_ecp_mul` on secp256r1 with a fixed scalar — isolates point multiplication.
3. `mbedtls_mpi_mul_mpi` on two fixed 256-bit operands — **the per-call cost at ECC size**, which is C3's crux.
4. `mbedtls_mpi_exp_mod` with 2048-bit and 4096-bit operands — confirms the accelerator still earns its place for RSA, and quantifies what turning it off costs there.
5. `mbedtls_mpi_inv_mod` on a 256-bit modulus — tests the inversion path (C1's remnant) in both versions.

Pin the benchmark task to **core 1** and stop the touch filter, to remove confound 5.4. Report min/mean/max, not just mean.

### 6.2 Counting calls without patching IDF

`mbedtls_mpi_mul_mpi` and `mbedtls_mpi_exp_mod` are global symbols the ESP port overrides, so they can be counted non-invasively with linker wrapping:

```cmake
target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=mbedtls_mpi_mul_mpi
    -Wl,--wrap=mbedtls_mpi_exp_mod)
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

Four builds minimum, all five benchmarks each:

| Run | IDF / mbedTLS | `HARDWARE_MPI` | Purpose |
|---|---|---|---|
| 1 | 6.0.2 / 4.1.0 | on | subject, as originally shipped |
| 2 | 6.0.2 / 4.1.0 | off | subject, as now shipped |
| 3 | 5.5 / 3.6.6 | on | reference, matching Arduino |
| **4** | **5.5 / 3.6.6** | **off** | **the missing cell (5.2)** |

Then repeat 1–4 with `ECP_FIXED_POINT_OPTIM=y` if the interaction in §2.1 needs explaining.

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
- **Any `sdkconfig.defaults` change means deleting `build/<app>/sdkconfig`** and a full ~280 s rebuild; defaults cannot override an existing generated config. A warm no-op build is ~4 s.
- **A Kconfig `choice` needs the current pick explicitly unset** (`# CONFIG_X_SIZE is not set` *then* `CONFIG_X_PERF=y`), and you must assert the value landed in the generated `sdkconfig` before believing any number. Both traps produced silently wrong measurements during the original work.
- Existing harness scripts worth reusing or adapting live in this session's scratchpad: `tls_cost.ps1` (timed request + watchdog counting), `perm_matrix.ps1` (config permutation driver). They are not in the repo; recreate or lift as needed.

### 6.6 Definition of done

The spike is finished when there is a table of per-operation timings and call counts for all four runs, and each of C2 and C3 is marked confirmed or refuted **with the number that decided it**. If the conclusion is "the accelerator was always wrong and the version gap is elsewhere", that is a successful spike — it corrects the record. A spike that merely reproduces the wall-clock numbers from §2 has not done its job.

---

## 7. Independent of the outcome

These are already committed and stand regardless of what the spike finds:

- The buffered `ClientReader` (§2.3) — a proven regression of ours, fixed.
- The config change to `HARDWARE_MPI` off + `FIXED_POINT_OPTIM` on — justified by §2.1's end-to-end measurements and validated by a full self-update, whatever the mechanism turns out to be.
- `LARGE_KEY_SOFTWARE_MPI`'s removal, and the reasoning in `sdkconfig.defaults` for why #119 stays safe.

What the spike can change is the *explanation* recorded in `sdkconfig.defaults` and [idf6-migration-continuation.md](idf6-migration-continuation.md), both of which currently assert C2 as though it were established. **If the spike refutes C2, those comments must be corrected.**
