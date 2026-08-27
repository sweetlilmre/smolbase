# Open question — where `SOC_MPI_HW_MIN_BIT_LEN` should live, and what value it should have

**Written:** 2026-08-27. **Status: undecided, deliberately parked.** Nothing is blocked by it except opening the esp-idf pull request.
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
