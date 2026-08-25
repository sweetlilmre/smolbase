# mbedTLS 4 TLS performance — spike record and handover

**Written:** 2026-08-24/25. **Read this first if you are picking this up with no context.** It is the single document for the TLS performance work: what was asked, what is measured and settled, what is still open, and how to continue.
**Branch:** `idf6-migration`. Working tree clean except the temporary probes under *Dirty state*, which are deliberate and still needed. **The IDF 6 SDK tree is instrumented too** — see that section before trusting any measurement.
**Companion:** [mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md) is the detailed evidence log for the constant-time strand and the upstream patch. [idf6-migration-continuation.md](idf6-migration-continuation.md) is the wider migration state.

> This file previously held the original spike's pre-registration (its plan, hypotheses and decision tables, written before any measurement). That text is preserved in git history; its conclusions are carried forward below. It was folded together with the handover note so there is one document rather than three.

---

## The two open questions — both now answered, and the answer is the same for both

They were: *where is the residual 1163 ms?* and *why did `mbedtls_x509_crt_verify` double?* Both were artefacts of one measurement error, described in full in **[Part 1a](#part-1a--the-ruler-was-wrong-scheduling-not-crypto)**. In short:

1. **`mbedtls_x509_crt_verify` has no chain-walk overhead to find.** Measured inside the library, it is **99.8% its two ECDSA signature verifies**. Parsing, name comparison, the CA-bit and key-usage checks, the hash of the TBS, the PSA key import and destroy — all of it together is **4 ms out of 1440 ms**. The "several hundred milliseconds of non-signature overhead" was arithmetic across two different rulers.
2. **The missing time is scheduling, not crypto.** An ECDSA verify costs 237 ms on core 1 and **418 ms on core 0**, same binary, same operands, same instant. Core 0 is where the WiFi driver and lwIP live; the crypto is being descheduled, not slowed down. `spike/mbedtls-perf/` pins its benchmark task to core 1 with WiFi absent, so **every number in it under-reads the running firmware by ~1.75×** — and every attribution built by subtracting a harness figure from a firmware figure inherited that error.

The practical consequence is larger than the bookkeeping one: **the same handshake to `api.github.com` takes 3.8 s on core 0 and 2.1 s on core 1.** See *What to do about core 0* below — the choice is a real trade-off, not a free win.

## State in one paragraph

The original question — why the arduino-esp32 build was faster than the native IDF build — is **answered with measurements rather than inference**. The whole gap is TLS handshake computation: network, HTTP framing, JSON parsing and Kconfig differences are each eliminated by direct measurement. Certificate chain verification is the largest single component, and it turns out to be *nothing but* its two ECDSA signature verifies — the "chain-walk overhead" that looked like the answer never existed, and the arithmetic that produced it mixed two incompatible rulers. The real correction is [Part 1a](#part-1a--the-ruler-was-wrong-scheduling-not-crypto): elliptic-curve maths on core 0 costs 1.75× what the same call costs on core 1, because the WiFi stack deschedules it, and a whole handshake goes 3.8 s → 2.1 s when moved. A separate strand produced a three-patch series now open as a **draft PR upstream**; it fixes a bignum constant-time regression that is a minority of the problem. There is now a working, instrumented arduino baseline that can be rebuilt and flashed at will — that capability did not exist before and is what made the comparison possible.

**What is still open.** Nothing about *our* build. What has not been done is re-running the corrected measurement on the arduino side: `spike_psa_vs_legacy.cpp`'s legacy half compiles unchanged against 3.6.6, so the same core-0/core-1 pair of ECDSA verifies can be measured in the v0.3.3 firmware, which would price the version gap on one ruler for the first time. That needs the arduino build flashed to the device, which displaces the current firmware.

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration` |
| ESP-IDF 6.0.2 | `~/esp/esp-idf` → mbedTLS 4.1.0. `& $HOME\esp\esp-idf\export.ps1` |
| ESP-IDF 5.5.5 | `~/esp/esp-idf-v5.5` → mbedTLS 3.6.6. `& $HOME\esp\esp-idf-v5.5\export.ps1` |
| **arduino baseline** | worktree `D:\source\smolbase-v033`, detached at `v0.3.3` (`22f1580`). Builds with `pio` (pioarduino, already installed). **Its probes are uncommitted — do not `git checkout` it.** |
| Device | ESP32-D0WD-V3 on **COM5**, RTS auto-reset. Running smolbase `0.4.0-dev`, shipped config. IP `10.0.0.32`. |
| Second device | ESP32-S3 on **COM9** — a Waveshare "xiaozhi" board, **not ours**, restored to its original firmware. Leave alone unless a second Xtensa target is needed. |
| Upstream clone | `D:\source\TF-PSA-Crypto`, branch `constant-time-embedded-perf`. `origin` = the fork, `upstream` = Mbed-TLS. Commit identity set locally to `sweetlilmre@gmail.com`. |
| MemSan clone | `~/ctflow/tfpsa` in WSL Ubuntu — TF-PSA-Crypto `development` plus the `framework` submodule, for constant-flow testing. |
| uncrustify 0.75.1 | `~/uncrustify-build/uncrustify/build/uncrustify` in WSL. The version `code_style.py` pins; Ubuntu's 0.78.1 is refused. |

Only one SDK can be active per shell, and `export.ps1` is not idempotent across versions — use a fresh shell to switch.

---

# Part 1 — The arduino-vs-ours question

## What is measured, and therefore settled

Same device, same network, same session, same config (RSA/MPI accelerator **ON**, fixed-point off) unless stated. Timed inside the firmware, not from the host.

| | arduino v0.3.3 (3.6.6) | ours (4.1.0) |
|---|---|---|
| TCP connect | ~40 ms | ~40 ms |
| **TLS handshake** | **~3300 ms** | **~5950 ms** |
| body (≈30 KB JSON) | ~197 ms | ~165 ms |

**The entire +2650 ms is handshake computation.** Eliminated by measurement, not argument:

- **Network** — a plain TCP connect to `api.github.com:443` is ~40 ms on both.
- **JSON and the buffered reader** — body read is 111–204 ms. `ClientReader` at `src/core/Http.cpp:31` has its 1436-byte buffer and works. (It once did not: see *The regression that was ours*.)
- **HTTP framing** — headers are 26–60 ms typically.
- **Kconfig** — every performance-critical mbedTLS option is identical in both generated `sdkconfig`s: `ECP_NIST_OPTIM=y`, `ECP_FIXED_POINT_OPTIM` unset, `HARDWARE_MPI=y`, `HARDWARE_SHA`/`AES=y`, `ECDSA_DETERMINISTIC=y`, `ECP_RESTARTABLE` unset, and `MPI_USE_INTERRUPT`/`ECP_WINDOW_SIZE`/`ECP_MAX_BITS`/`MPI_WINDOW_SIZE` absent on both. Re-check with:

```
diff <(grep -E '^(CONFIG_MBEDTLS|# CONFIG_MBEDTLS)' ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig | sort) \
     <(grep -E '^(CONFIG_MBEDTLS|# CONFIG_MBEDTLS)' build/smolbase/sdkconfig | sort)
```

### Attribution of the gap

**Superseded — kept because the numbers in it are still real, but the arithmetic is not.** The "how" column mixes two rulers: the first row is a firmware measurement on core 0, the second is a primitive benchmark on a core-1 task with no WiFi, and [Part 1a](#part-1a--the-ruler-was-wrong-scheduling-not-crypto) measures the difference between those contexts at 1.75×. The residual is an artefact of the mismatch.

| cause | ms | how |
|---|---|---|
| `x509_crt_verify` chain walk | **1257** | same DER compiled into both builds, application-level benchmark |
| remaining EC ops (ServerKeyExchange verify + 2 × `ecp_mul`) | 230 | primitive benchmarks — **on the wrong ruler, multiply by ~1.75** |
| **explained** | **1487** | |
| **residual — was open question 1** | **1163** | not a residual: scheduling on core 0 |

### X.509, identical certificate bytes in both builds

GitHub's live chain was dumped from the device as DER (three certificates: 1009, 867, 842 bytes) into `src/core/spike_chain.h` and compiled into both builds, so the same bytes go through the same public mbedTLS calls. The arduino build's mbedTLS is precompiled and cannot be instrumented, which is why this is an application-level benchmark rather than library instrumentation.

| | arduino 3.6.6 | ours 4.1.0 |
|---|---|---|
| `x509_crt_parse_der` × 3 certs | 8.5 ms | 5.5 ms |
| **`x509_crt_verify`** | **1270 ms** | **2527 ms** |

Parsing is irrelevant and 4.1.0 is *faster* at it. The doubling looked like it was inside the chain walk; [Part 1a](#part-1a--the-ruler-was-wrong-scheduling-not-crypto) shows there is no chain walk to speak of — `x509_crt_verify` is 99.8% its two signature verifies, in both the measurement and the source.

### Handshake shape (shipped config, `MBEDTLS_DEBUG` level 3)

`SERVER_CERTIFICATE → SERVER_KEY_EXCHANGE` is 2445 ms, ~58% of the handshake. Within it: 1481 ms before the bundle callback, 5 ms bundle lookup over 146 certs, 949 ms root signature verify (`sig_md=10`, SHA-384, so P-384). The other blocks: `SERVER_KEY_EXCHANGE → CERTIFICATE_REQUEST` 520 ms, `CLIENT_KEY_EXCHANGE → CERTIFICATE_VERIFY` 785 ms, everything else under 140 ms each.

IDF 6 rewrote `esp_crt_check_signature()` to use PSA (`mbedtls_pk_get_psa_attributes`, `psa_import_key`, `psa_verify_hash` — a key import per verification) where IDF 5.5 called `mbedtls_pk_verify_ext` with no PSA at all. Substituting the older approach drops that step 949 → **865 ms**, so PSA costs ~84 ms. The rewrite was probably forced, not chosen: mbedTLS 4 removed `sig_opts` and changed `pk_verify_ext`'s signature.

### The original symptom, reproduced by accident

Running five chain verifications back to back on the **arduino** build triggered the task watchdog and panicked the device — arduino-esp32 ships `CONFIG_ESP_TASK_WDT_PANIC=y`. That is precisely the failure this whole investigation started from. The old firmware was closer to that edge than anyone realised: 1270 ms per chain verification against a 5 s budget shared with everything else.

---

# Part 1a — The ruler was wrong: scheduling, not crypto

All of this is one session on one binary, 2026-08-25, in the shipped configuration (`HARDWARE_MPI` off, `ECP_FIXED_POINT_OPTIM` on). Every figure is reproduced across two consecutive `/api/update/check` requests.

## Taking `x509_crt_verify` apart from the inside

The arduino build's mbedTLS is precompiled, but ours is not, so the chain walk was instrumented directly: cycle counters (`rsr ccount`, no component dependency) around `psa_hash_compute`, `mbedtls_pk_can_do_psa`, `mbedtls_pk_verify_ext` and `x509_crt_check_parent` in `library/x509_crt.c`, and around `psa_import_key` / `psa_verify_hash` / `mbedtls_ecdsa_der_to_raw` / `psa_destroy_key` in `tf-psa-crypto/extras/pk_wrap.c`. The counters are zeroed immediately before the benchmark's single `mbedtls_x509_crt_verify`, because free-running from boot they read every handshake since.

| inside one `mbedtls_x509_crt_verify` (1436 ms) | | calls |
|---|---|---|
| `mbedtls_pk_verify_ext` | **1434.9 ms** | 2 |
|  → `psa_verify_hash` | **1432.3 ms** | 2 |
|  → `psa_import_key` | 2.1 ms | 2 |
|  → `psa_destroy_key` | 0.21 ms | 2 |
|  → `mbedtls_ecdsa_der_to_raw` | 0.13 ms | 2 |
| `psa_hash_compute` (TBS) | 1.0 ms | 2 |
| `mbedtls_pk_can_do_psa` | 0.15 ms | 2 |
| `x509_crt_check_parent` | 0.10 ms | 3 |
|  → `x509_name_cmp` | 0.05 ms | 3 |

**There is nothing else in there.** The chain walk, the name comparison, the CA-bit and key-usage checks and the per-certificate PSA key import together cost 0.3% of the call. The earlier "~1370 ms of chain-walk overhead" came from subtracting `spike/mbedtls-perf/`'s primitive figures from a firmware measurement, and those two numbers are not on the same scale.

## PSA is not the culprit either

mbedTLS 4 routes every certificate signature check through PSA; the arduino build's 3.6.6 does not have `MBEDTLS_USE_PSA_CRYPTO` defined at all (checked in `framework-arduinoespressif32-libs/esp32/include/.../mbedtls_config.h`) and so uses the legacy `mbedtls_ecdsa_verify`. That looked like a promising asymmetry. It is not one. `src/core/spike_psa_vs_legacy.cpp` verifies the *same* signature both ways in the same binary, on the RFC 6979 keys the harness uses:

| | legacy `mbedtls_ecdsa_verify` | PSA import+verify+destroy | ratio |
|---|---|---|---|
| P-256, core 1 | 237.4 ms | 240.3 ms | 1.01 |
| P-384, core 1 | 504.5 ms | 508.6 ms | 1.01 |

The two APIs cost the same to within 1%. The PSA route adds a key import and a destroy, and together they are ~2 ms.

## Where the time actually goes: core 0

The same benchmark, same binary, same operands, differing only in which task runs it:

| P-256 ECDSA verify | legacy | PSA |
|---|---|---|
| pinned core 1, priority 5 | **237 ms** | 240 ms |
| pinned core 0, priority 5 | **418 ms** | 419–492 ms |
| on the httpd task (unpinned, behaves as core 0) | 406–506 ms | 411–496 ms |
| **pinned core 0, priority 24** | **236.7 ms** | 240.2 ms |
| `spike/mbedtls-perf/` harness, `idf6-mpi-off-fp` | 234 ms | — |

Two things fall out. The harness figure and the core-1 figure agree to 1.5%, so **the harness was always right** — it just measures a context the firmware never runs in. And at priority 24, above the WiFi task (23) and the lwIP task (18), core 0 becomes exactly as fast as core 1. **The crypto is not slower on core 0; it is descheduled.** Roughly 44% of the wall clock of a certificate verify on core 0 is spent running the network stack instead — and this is with WiFi merely associated and idle, no TLS traffic in flight, since the benchmark touches no socket.

## And it carries through to a whole handshake

Three full `esp_tls_conn_new_sync` handshakes to `api.github.com`, A-B-A in one session because cross-session comparisons here are worthless, repeated on two consecutive requests:

| | run 1 | run 2 |
|---|---|---|
| core 0 (A) | 3869 ms | 3771 ms |
| **core 1 (B)** | **2147 ms** | **2132 ms** |
| core 0 (A again) | 3710 ms | 3800 ms |

**A 44% reduction from a scheduling decision.** For scale, the arduino v0.3.3 baseline this whole investigation was chasing is a 3.3 s handshake, and it runs on core 0 too — `Web.cpp:118` in the v0.3.3 worktree pins httpd to core 0 exactly as `Web.cpp:147` does today, so this is not the arduino-vs-ours difference. It is a cost both builds have always paid.

## What to do about core 0

Not a free win, and not this document's call to make. `httpServer.config.core_id = 0` is deliberate — ADR 0001 puts network work on core 0 precisely so consumer code keeps core 1. Moving a 2.1 s handshake onto core 1 moves a 2.1 s stall onto the task that drives the panel. Three options, none yet measured against the App loop:

- **Run only the update/TLS work on core 1**, on its own task, leaving the rest of the web server on core 0. Buys the 1.7 s; costs the App loop whatever it costs.
- **Raise the priority of the task doing the handshake** while it handshakes. Measured to be exactly as effective, and considerably more dangerous: priority 24 starves the WiFi task, and the benchmark held it for 240 ms at a time, not 2 s.
- **Accept it.** 3.8 s with no watchdog trips is a working system, and this is one HTTP request a day.

## What this changes about everything above

Every attribution in this document that subtracted a `spike/mbedtls-perf/` figure from a firmware measurement is understated by roughly the same 1.75×, including the 1487 ms "explained" row and the 230 ms attributed to the remaining EC operations. The primitive numbers themselves are unaffected — they are correct, internally consistent, and reproduce exactly when the firmware runs the same call in the same context. What was wrong was treating them as the price the firmware pays.

**The rule this earns:** a primitive benchmark measures a primitive, not a program. Before subtracting one from the other, measure the same call in both contexts and find out what the context costs.

---

# Part 2 — The original spike: the accelerator and the constant-time regression

This is the strand that ran first. Its conclusions stand; its scope was too narrow, which is the lesson in *What the spike got wrong*.

## Primitive benchmarks

Offline, no WiFi, no TLS, no network peer. Fixed operands, one task pinned to core 1, `esp_timer`, batches sized to ~200 ms, min/mean/max over 5 batches. Harness: `spike/mbedtls-perf/`.

**Accelerator ON, same harness build** (the arduino build's configuration):

| bench | 3.6.6 | 4.1.0 | gap |
|---|---|---|---|
| `ecdsa_verify_p256` | 297.88 ms | 416.92 ms | +40.0% |
| `ecp_mul_p256` | 139.55 ms | 195.19 ms | +39.9% |
| `ecdsa_verify_p384` | 519.25 ms | 738.58 ms | +42.2% |
| `ecp_mul_p384` | 240.20 ms | 341.79 ms | +42.3% |

**P-384 is not disproportionately affected** — 42% against 40%. The hypothesis that the larger curve carried more of the regression is refuted.

## What the spike set out to test, and what it found

Two claims were pre-registered and both are now settled.

**C2 — does mbedTLS 4's ECP call `mbedtls_mpi_mul_mpi` more often than 3.6.6? REFUTED.** Both versions make **exactly 5817 calls per P-256 verify** (3912 with fixed-point ECP), and exactly 2905 per `ecp_mul` (1000 with it). Identical to the digit, in every lever combination, on both SDKs. A count cannot be distorted by timing overhead, so this is the cleanest result in the work. It also shows the mechanism of the fixed-point win: it cuts call *count*, where turning the accelerator off cuts cost *per call*.

**C3 — at ECC operand sizes, does the accelerator cost more than a software multiply? CONFIRMED.** `mpi_mul` mean µs per call, unhooked:

| operand bits | 3.6.6 software | 3.6.6 accelerator | 4.1.0 software | 4.1.0 accelerator |
|---|---|---|---|---|
| **256** | **9.78** | **19.49** | **11.28** | **19.69** |
| 512 | 29.22 | 22.15 | 32.05 | 22.29 |
| 1024 | 100.80 | 36.04 | 106.56 | 36.18 |
| 2048 | 378.74 | 76.51 | 391.29 | 76.65 |
| 4096 | 1467.20 | 457.92 | 1496.11 | 462.30 |

At 256 bits the peripheral costs almost exactly twice a software multiply. The software column scales with the square of operand size; the accelerator column is nearly flat from 256 to 1024 — that flatness *is* the fixed per-call ceremony, exactly as the ESP port's lack of a lower size threshold predicts. The crossover is between 256 and 512 bits, and P-256 sits on the losing side.

Above the crossover it earns its place. `mpi_exp_mod` with `e = 65537`:

| | 3.6.6 sw | 3.6.6 hw | 4.1.0 sw | 4.1.0 hw |
|---|---|---|---|---|
| 2048-bit | 61.77 ms | 17.03 ms (**3.6×**) | 74.50 ms | 19.98 ms (**3.7×**) |
| 4096-bit | 227.03 ms | 54.56 ms (**4.2×**) | 262.85 ms | 65.67 ms (**4.0×**) |

So the shipped config gives up **+197 ms per RSA-4096 verify**. Affordable because a chain does one or two such verifies while an ECDHE handshake does thousands of 256-bit multiplies.

**The accelerator was always wrong for this workload, in both versions.** The cell the precompiled arduino libraries made unreachable, measured at last (uninstrumented builds, fixed-point off):

| | accelerator on | accelerator off | gain |
|---|---|---|---|
| **mbedTLS 3.6.6** | 298.22 ms | **245.12 ms** | **−17.8%** |
| **mbedTLS 4.1.0** | 366.86 ms | 327.75 ms | −10.7% |

3.6.6 gains *more* from turning it off than 4.1.0 does. So the accelerator explains none of the version gap, and the shipped config change was right for a reason that has nothing to do with mbedTLS 4.

## The end-to-end matrix that drove the config change

Historical, with the buffered reader in place, wall-clock of one `/api/update/check`:

| `HARDWARE_MPI` | `ECP_FIXED_POINT_OPTIM` | handshake | watchdog |
|---|---|---|---|
| y | n | 6.85 s | trips every request |
| y | y | 6.50 s | trips every request |
| n | n | 5.92 s | trips every request |
| **n** | **y** | **4.25 s** | **clean** |
| *arduino v0.3.3* | | *4.12 s* | *clean* |

Offline, the two levers are **independent multiplicative factors** (predicted ×0.621 against measured ×0.625 on v4). The large interaction this table appears to show is most likely an artefact of the watchdog tripping in three cells and not the fourth.

## The regression that was ours

`core/Http`'s `ClientReader` was **unbuffered** — one byte per `esp_http_client_read`, through mbedTLS, while ArduinoJson scans byte by byte. GitHub's ~30 KB release JSON became ~30,000 TLS round trips, measured at **2.07 s** of the original 8.49 s. The arduino path never had this cost because it passed an arduino-esp32 `NetworkClient` Stream, which owns a 1436-byte `NetworkClientRxBuffer` for exactly this purpose. Fixed at the same size, in the same place. This means the headline 8.49 s vs 4.12 s comparison conflated two independent regressions, only one of which was ever about mbedTLS.

## The constant-time root cause, and the upstream patch

Three lines, changed in 2024, are the whole primitive-level version gap: `mbedtls_mpi_core_sub()`'s loop and `mbedtls_mpi_core_mla()`'s carry tail now call `mbedtls_ct_uint_lt()` + `mbedtls_ct_mpi_uint_if()` where 3.6.6 used a plain comparison. Reverting those three lines recovers ~100% of the primitive gap in **both** accelerator configurations. `mbedtls_ct_uint_lt()` has hand-written assembly for Arm, AArch64, x86-64 and x86 and generic barrier-based C for everything else; at `-Os` it is also left out of line, on Arm as well as Xtensa. Full detail, disassembly and the constant-flow results are in [mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md).

A C-level fix was tried and **refuted by constant-flow testing** — clang re-derives the comparison from a branchless idiom and emits a branch. The shipped answer is an Xtensa assembly path plus forced inlining.

## A harness defect the spike found in itself

`mpi_mul_hooked` appeared to show the instrumentation costing 11.08 µs per call with the accelerator on and 2.33 µs with it off — an artefact the same order as the effect under test, pointing the same way, and capable of manufacturing the whole result. Four uninstrumented control cells (`-D SPIKE_NO_WRAP=1`) settled it: the **real** hook cost is **0.21–1.30 µs per call**, and a build with no hook linked at all reproduces the same margin. The margin is benchmark ordering — `mpi_mul_hooked` runs after the 4096-bit sweep and shares an oversized destination MPI. **`mpi_mul_hooked` does not price the instrumentation; the no-wrap control does.** Fixing the ordering is an open follow-up that affects nothing above.

---

# Part 3 — Working on this

## How to reproduce anything here

### The arduino baseline

```
cd D:\source\smolbase-v033
$env:PATH = "$HOME\.platformio\penv\Scripts;$env:PATH"
pio run -e smolbase
pio run -e smolbase -t upload --upload-port COM5
```

Probes log to serial with `[spike]` / `[spike-x509]`. **The prebuilt `v0.3.3` release binary fails today** (`could not reach GitHub releases`); the source build works. Do not use the release artifact.

### Our build

```
& $HOME\esp\esp-idf\export.ps1
cd D:\source\smolbase
idf.py '@smolbase.args' build
idf.py '@smolbase.args' -p COM5 -b 460800 flash
```

Probes log with the `spike` tag at WARN. To re-enable the handshake timeline add `CONFIG_MBEDTLS_DEBUG=y`, `# CONFIG_MBEDTLS_DEBUG_LEVEL_WARN is not set` and `CONFIG_MBEDTLS_DEBUG_LEVEL_DEBUG=y` to `sdkconfig.defaults`, then **delete `build/smolbase/sdkconfig`** so the defaults take.

### Reading the serial

A default `SerialPort` drops bytes during a burst and produces plausible-but-wrong text. Use `ReadBufferSize >= 262144` and a tight `ReadExisting()` loop doing nothing else inside it.

### Primitive benchmarks

`spike/mbedtls-perf/`, with its own [README](../../spike/mbedtls-perf/README.md). `run.ps1 -Runs <tag> -Flash`; captures land in `results/` and collate to `results.csv`. **Flashing it erases the smolbase firmware** — own bootloader and `single_app` partition table. Recover with the full serial flash above. WiFi credentials survive in practice: NVS sits at `0x9000` in both layouts and the spike never writes it.

### Constant-flow testing

```
bash spike/mbedtls-perf/upstream/constant-flow/cf_configure.sh
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh revert   # negative control, must FAIL
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh stock    # must PASS
```

### Upstream code style

```
bash spike/mbedtls-perf/upstream/constant-flow/build_uncrustify.sh   # 0.75.1, once
cd D:\source\TF-PSA-Crypto
python3 framework/scripts/code_style.py --fix --uncrustify ~/uncrustify-build/uncrustify/build/uncrustify
```

`constant-flow/rebuild_series.sh` rebuilds the whole three-commit series from `development` with the style fix applied per stage.

## Dirty state that is deliberate — revert before the branch ships

| file | what |
|---|---|
| `src/core/Http.cpp` | phase split (open/headers/body), TCP-vs-TLS connect split, the core-0/core-1/core-0 handshake matrix, and the three bench calls — `SPIKE` markers throughout |
| `src/core/spike_x509.inc` | the X.509 benchmark, identical source in both builds, plus the library-counter dump |
| `src/core/spike_chain.h` | GitHub's live chain as DER, captured 2026-08-25 |
| `src/core/spike_psa_vs_legacy.cpp` | PSA-vs-legacy ECDSA verify and the core/priority matrix. Own translation unit because `MBEDTLS_ALLOW_PRIVATE_ACCESS` must precede every mbedTLS include and `Http.cpp` has already pulled in `esp_tls.h` |
| `D:\source\smolbase-v033` | the same files plus instrumentation in `src/core/GhUpdate.cpp`, **uncommitted** |

**The IDF 6 SDK tree is no longer pristine.** `library/x509_crt.c` and `tf-psa-crypto/extras/pk_wrap.c` under `~/esp/esp-idf/components/mbedtls/mbedtls` carry the cycle counters described in Part 1a; every addition is marked `SPIKE`. Revert with `git checkout` in that tree when done. The IDF 5.5 tree is untouched. Always check `git status` in both before trusting a measurement.

## Traps that cost real time

- **`idf.py flash` rebuilds.** The tree must be in the intended state at *flash* time, not just build time. A patched cell was built, the patch reverted, and the flash step silently recompiled it unpatched; the numbers came back byte-identical to the unpatched run, which is the only reason it was caught. **Disassemble the flashed ELF and confirm the change is in it.**
- **`xtensa-esp-elf-objdump` sometimes decodes an ELF as raw hex words.** `grep -c call8` then returns 0, indistinguishable from "no calls". A tool returning zero matches is not a tool returning a zero answer.
- **A primitive benchmark is not a program.** `spike/mbedtls-perf/` pins to core 1 with no WiFi; the firmware runs the same call on core 0 with the WiFi task at priority 23 above it, and pays 1.75x. Both numbers are correct. Subtracting one from the other produced a 1163 ms "residual" and a phantom chain-walk overhead, and cost most of two sessions.
- **Cross-session comparisons are worthless here.** Two conclusions were wrong because a fresh measurement was compared against a number recorded earlier. Measure A-B-A in one session.
- **Validate the response body on every timing rep.** Several runs used `curl -o NUL` and measured only elapsed time; a failing request times differently from a succeeding one. Check for `"latest"`.
- **Numbers move when the binary changes.** Adding P-384 benchmarks shifted the *P-256* result on 4.1.0 by 13.6% with no change to that code path — flash-cache pressure. This is why the version gap reads 23% in one build and 40% in another; both are correct for their own binary. **Compare only within one build.**
- **Heredocs mangle escape sequences.** `\n` and `\\` inside `python - <<'PY'` have been corrupted repeatedly, once producing a broken C `#define`. Use the Write/Edit tools for anything with escapes.
- **WSL `/tmp` is cleared** when the instance restarts, which happens between tool calls. Use `$HOME`.
- **WSL is behind the same corporate TLS interception as Windows.** `openssl s_client` from either shows Netskope's chain. Only the device sees GitHub's real chain — dump it from there.
- **`curl` in PowerShell cannot read Git Bash paths** (`/tmp/...`): exit 26, uploads nothing, silently.
- **`sdkconfig.defaults` cannot override an existing generated `sdkconfig`.** Delete `build/<app>/sdkconfig` after editing it, and assert the value landed before believing a measurement.

## What the spike got wrong, and why it matters for what is left

The spike measured bignum primitives exhaustively and never asked what fraction of a handshake they are. **They are under half.** Two of its conclusions also had to be corrected mid-flight — a mechanism asserted from inference (C2) turned out false, and a "per-call overhead" reading of the multiply data was actually an O(n) per-limb effect.

The same failure mode is the live risk for both open questions: **size a component before benchmarking it.** The phase split in `core/Http.cpp` and the `MBEDTLS_DEBUG` timeline exist for exactly that.

## What is left, in the order it is worth doing

1. **Price the version gap on one ruler.** `src/core/spike_psa_vs_legacy.cpp`'s legacy half needs no changes to compile against 3.6.6 — the header compatibility is already handled the way `spike/mbedtls-perf/main/compat.h` handles it. Drop it into `D:\source\smolbase-v033`, flash, and read P-256 and P-384 verify on core 0 and core 1. That is the first arduino-vs-ours number that will not be contaminated by execution context. It displaces the current firmware from the device; recover with the full serial flash in *How to reproduce anything here*.
2. **Decide what to do about core 0** (see *What to do about core 0* in Part 1a). Measure whatever option is chosen against the App loop's frame budget, not just against the handshake.
3. **Re-derive the attribution table** in Part 1 once (1) is in. Every row built by subtracting a harness figure from a firmware figure is understated by roughly 1.75×.
4. **Fix the benchmark ordering defect** in `mpi_mul_hooked` (Part 2). Affects nothing above; it is loose ends.

The `MBEDTLS_DEBUG` handshake timeline is still the right tool for sizing anything inside the handshake that is not certificate work, and Part 1a says nothing about the ECDHE key exchange or the record layer beyond the general 1.75× correction.

## Upstream PR — separate strand, do not conflate

**[Mbed-TLS/TF-PSA-Crypto#873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873), draft.** Three commits: forced inlining where an asm path exists, a new Xtensa asm path, and `_if_else_0` at two call sites. CI code style passes. The body carries the measurements, the constant-flow results with a working negative control, an explicit not-verified list, and the measured **~875 ms regression** the series causes on ESP32 builds with the RSA/MPI accelerator enabled.

It fixes the primitives, and the primitives are under half the problem. Findings from the two open questions are likely to be **more** valuable upstream than what is currently in that PR, and should probably be reported separately rather than folded into it.
