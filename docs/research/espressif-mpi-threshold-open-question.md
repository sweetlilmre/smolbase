# Open question — where `SOC_MPI_HW_MIN_BIT_LEN` should live, and what value it should have

**Written:** 2026-08-27. **Status: the measurement half is ANSWERED (2026-08-30, see the S3 section at the end); the naming/location half (A/B/C below) remains a call for the PR review.** The PR was opened as draft without waiting: [espressif/esp-idf#19027](https://github.com/espressif/esp-idf/pull/19027).
**Context:** [espressif-hardware-mpi-report-draft.md](espressif-hardware-mpi-report-draft.md) is the PR description. [mbedtls4-perf-spike.md § open item 3](mbedtls4-perf-spike.md) is the wider record. The branch is [`sweetlilmre/esp-idf:fix/mpi-hw-min-bit-len`](https://github.com/sweetlilmre/esp-idf/tree/fix/mpi-hw-min-bit-len) — one commit, pushed, **no PR opened**.

## The line in question

The patch adds this to `components/mbedtls/port/bignum/esp_bignum.c`:

```c
#ifndef SOC_MPI_HW_MIN_BIT_LEN
#define SOC_MPI_HW_MIN_BIT_LEN 512
#endif
```

Multiplies below 512 bits go to software; at or above, to the RSA peripheral. `#ifndef` means another header can override the value, and nothing currently does.

## Three problems, in order of how much they matter

**1. It can make another chip slower.** 512 is the measured crossover on an **ESP32-D0WD-V3 only**. Every other Espressif part has different multiply hardware, a different core, or both, and none were measured. If another chip's crossover is *below* 512, this patch routes multiplies to software that its hardware would have done faster — a regression on that part. If it is *above* 512, the patch simply helps less than it could. The first case is a real risk and the PR description must say so.

**2. The name promises something that is not there.** `SOC_`-prefixed names in ESP-IDF normally live in `components/soc/<chip>/include/soc/soc_caps.h`, one file per chip. This one does not; it sits in the port with a default. A reviewer will look for it in `soc_caps.h` and not find it.

**3. No chip actually overrides it.** So in practice every target silently gets 512 whether that suits it or not.

## The options

| | what | cost | risk |
|---|---|---|---|
| **A** | Leave as is | none | Ships a `SOC_` name with no `soc_caps.h` entry, and an unmeasured default on every non-ESP32 part |
| **B** | Rename to `ESP_MPI_HW_MIN_BIT_LEN`, keep the `#ifndef`, state the ESP32-only measurement plainly in the PR | ~10 min, no new measurement | Honest, but still one value for all chips |
| **C** | Add a per-chip value to `soc_caps.h` | touches many files, needs a measurement per chip | Correct end state; we do not have the hardware |

**Recommendation: B.** It is honest about what was measured, does not claim a chip capability nobody supplied, stays a one-file change a maintainer can accept, and leaves Option C to Espressif, who have the parts. Add one sentence to the PR: *this default is measured on the ESP32 and could reduce performance on a part whose crossover is lower; the measurement is one build and one flash per chip and I am happy to run it on an S3.*

## What would turn a guess into a number

Measuring the ESP32-S3 crossover would halve the unknown. The method is the one already used, and takes one build and one flash:

1. `spike/mbedtls-perf/`, an `idf6-s3-mpi-on` style cell with the accelerator **on**, no `--wrap`.
2. Read `mpi_mul` at 256 and 512 bits, then the same cell with the accelerator **off**.
3. The crossover is where the two curves meet.

**The S3 on COM9 is not ours** — the records say it is a Waveshare board restored to its original firmware. Flashing it needs the owner's say-so first.

## To resume

Pick A, B or C. For B: rename the constant, amend the commit on the fork branch, update the PR description, then open the PR. No measurement needed.

## ANSWERED — the ESP32-S3 crossover, measured 2026-08-30

A user-supplied ESP32-S3 (LX7, MAC `28:84:85:8d:31:b4`, COM9 — not the Waveshare board the caution above was about) ran the prescribed measurement: harness cells `idf6-s3-mpi-{on,off}` plus `idf6-s3-mpi-on-fix` with the PR's patch applied, IDF 6.0.2, 240 MHz, a 384-bit point added to the sweep for the purpose. Captures in `spike/mbedtls-perf/results/`, fix image fingerprinted (`mbedtls_mpi_mul_mpi` 496 B, gate + fallback in).

µs per `mbedtls_mpi_mul_mpi`, mean of 5:

| bits | software | hardware | hw on, with the fix |
|---|---|---|---|
| 256 | 8.74 | 15.76 | **9.15** |
| 384 | 16.38 | 19.34 | **16.79** |
| 512 | 23.55 | 23.64 | 23.64 |
| 1024 | 75.78 | 49.10 | 49.11 |
| 2048 | 270.67 | 138.26 | 138.28 |
| 4096 | 1022.25 | 671.98 | 673.70 |

**The S3 crossover is at 512 bits almost exactly** (23.55 against 23.64 — a dead heat), with software clearly ahead at 256 (1.8×) and 384 (1.18×). So the 512 threshold is *correct on both measured Xtensa parts*, and problem 1 above ("it can make another chip slower") is retired for the S3: there is no size the fix routes wrongly. The fallback's own gate costs ~0.4 µs at 256 bits (9.15 against 8.74 pure-software).

Operation level, same story as the ESP32 but smaller — the S3's peripheral is less bad at small operands, so there is less to win:

| bench | hw on | with the fix | hw off |
|---|---|---|---|
| `ecdsa_verify` P-256 | 302.09 ms | **272.19 ms** (−9.9%) | 267.11 ms |
| `ecdsa_verify` P-384 | 575.44 ms | **565.28 ms** | 555.61 ms |
| `mpi_exp_mod` 2048 | 20.62 ms | 20.33 ms | 54.96 ms |
| `mpi_exp_mod` 4096 | 74.43 ms | 74.11 ms | 189.77 ms |

ECC lands within 2–5% of software speed and RSA keeps its 2.7× hardware advantage, in one build — the fix behaves on the S3 exactly as it does on the ESP32. **Recommendation B stands, now with data:** one shared default of 512 is measured-correct on ESP32 and ESP32-S3, the two chips that actually have this peripheral generation; the rename question is purely cosmetic and is flagged in the PR body's "Notes for reviewers".
