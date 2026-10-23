# Open question — where `SOC_MPI_HW_MIN_BIT_LEN` should live, and what value it should have

**Written:** 2026-08-27. **Status: MEASURED 2026-09-07; a methodology error found and corrected 2026-09-10 — read *Methodology correction* before trusting any percentage below it.** In short: the [#19027 review](https://github.com/espressif/esp-idf/pull/19027#issuecomment-5534987755) supplied a nine-part crossover table that disagreed with this project's 512, and 14 cells across both boards found the disagreement was ours — we had been measuring an mbedTLS carrying the TF-PSA-Crypto #873 series, which speeds the software multiply by 6–12% and lifts the crossover ~5%. On a clean tree we get 383 bits against the reviewer's 374. The reviewer's requested stack-copy change is confirmed good on both parts and should be adopted. The consequential finding is that **#873 flips 384-bit operands from the hardware side to the software side**, so a constant chosen against today's mbedTLS will be wrong once #873 merges. Options A/B/C below are superseded. The PR is open, out of draft, awaiting review: [espressif/esp-idf#19027](https://github.com/espressif/esp-idf/pull/19027).
**Context:** [mbedtls4-perf-spike.md § open item 3](mbedtls4-perf-spike.md) is the wider record. (A `espressif-hardware-mpi-report-draft.md` carrying the PR description used to be linked here; draft correspondence was never meant to be committed and was rewritten out of history on 2026-09-14, after its findings were folded into this file.) The branch is [`sweetlilmre/esp-idf:fix/mpi-hw-min-bit-len`](https://github.com/sweetlilmre/esp-idf/tree/fix/mpi-hw-min-bit-len). ("No PR opened" stood here until 2026-09-07 and was stale — #19027 has been open since; the sections below it are dated for the same reason.)

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

## REOPENED — the reviewer measured it on nine parts and does not get 512 (2026-09-07)

[Ashish285's review comment](https://github.com/espressif/esp-idf/pull/19027#issuecomment-5534987755) reproduces the improvement and then supplies a crossover table across targets **and CPU frequencies**, bits:

| target | 48 | 64 | 80 | 96 | 120 | 160 | 240 | 320 | 400 |
|---|---|---|---|---|---|---|---|---|---|
| esp32 | | | 364 | | | 369 | 374 | | |
| esp32s2 | | | 261 | | | 286 | 325 | | |
| esp32s3 | | | 348 | | | 399 | **508** | | |
| esp32c3 | | | 187 | | | 269 | | | |
| esp32c5 | | | 264 | | | 356 | 575 | | |
| esp32c6 | | | 284 | | 335 | 389 | | | |
| esp32s31 | | | | | | | 556 | 609 | |
| esp32h2 | 329 | 346 | | 569 | | | | | |
| esp32p4 | | | | | | | | | 603 |

**The S3 row is not a disagreement.** Their 508 at 240 MHz against the 512 measured above is a dead heat by anyone's arithmetic, and this project's own S3 numbers (23.55 software against 23.64 hardware at 512) said "dead heat" too.

**The ESP32 row is a partial disagreement, and smaller than it looks.** 512 was never a measured ESP32 crossover — it is a round number chosen above the largest size where software was measured to win. Interpolating this project's own ESP32 sweep puts the crossing at ~408 bits:

| bits | software | hardware | software − hardware |
|---|---|---|---|
| 256 | 9.78 | 19.49 | −9.71 |
| 384 | 21.80 | 23.40 | −1.60 |
| 512 | 29.22 | 22.15 | **+7.07** |

so the sign change is between 384 and 512, at roughly 384 + 128 × (1.60 / 8.67) ≈ 408. Their 374 and this 408 are ~9% apart, and both are far below the 512 in the patch. Two measurements of the same quantity that agree to 9% across different harnesses is close to as good as this gets.

### Where it actually matters: 384 bits, and nowhere else

The threshold is not a continuous quantity in any workload. Only a handful of operand sizes ever reach `mbedtls_mpi_mul_mpi`: **256** (P-256 ECC), **384** (P-384 ECC — GitHub's chain has a P-384 root, so this one is not hypothetical for smolbase), **512**, and RSA's ≥1024. A threshold anywhere in the open interval (384, 512] produces *identical routing* for every one of them. So 408 versus 512 is a distinction without a difference, and the only question that has consequences is:

**does 384 route to software or to hardware?**

Both this project's measurements say software wins at 384, on both chips:

| | software | hardware | software wins by |
|---|---|---|---|
| ESP32 @ 240 | 21.80 | 23.40 | 7% |
| ESP32-S3 @ 240 | 16.38 | 19.34 | 18% |

The reviewer's ESP32 figure of **374 puts 384 on the hardware side**, which by our numbers makes a P-384 operand ~7% slower. Their S3 figure of 508 at 240 MHz puts it on the software side, agreeing with us — but their S3 figure at **80 MHz is 348, which puts 384 on the hardware side again**, where our 240 MHz data says software is 18% ahead.

That is the whole disagreement, stated in the only terms that change a routing decision. It is worth one measurement rather than one argument, which is what `spike/mbedtls-perf`'s `*-xover-*` cells are for: a 32-bit-step sweep from 256 to 512, hardware and software columns from the same binary, at 80/160/240 MHz on both chips.

### Consequence for the constant: it has to vary, and it has to vary in the right units

Their table settles the substance of problem 1 above ("it can make another chip slower") in the affirmative — the crossover ranges from **187** (C3 at 80 MHz) to **609** (an S3 variant at 320 MHz), so a single 512 is wrong for most parts, and wrong in the dangerous direction (too high, routing to software what the hardware would do faster) for the C3, S2, C6 and H2 at low clocks.

It also adds a dimension the original three options did not have: **CPU frequency**. The mechanism is not mysterious. The software multiply is entirely CPU-bound, so its cost scales as 1/f. The peripheral's cost is partly CPU-bound (crypto lock, clock enable, register writes, polling) and partly fixed at the peripheral's own clock, which on these parts does not rise with the CPU. Speeding the CPU up therefore makes software cheaper faster than it makes the peripheral cheaper, and the crossover rises. How much depends on how large the fixed part is: on the ESP32 the two nearly cancel (364 → 374, 3% across a 3× clock change), on the S3 they do not (348 → 508, 46%).

So `soc_caps.h` alone — option C above — cannot express this. A `SOC_` constant is a property of the silicon; this is a property of the silicon *and* the build's clock.

### Proposal: a per-target table keyed on frequency, chosen to classify 384 correctly

```c
/* Below this operand size the hardware unit costs more than a software
   multiply. Two things set it, which is why it is not a plain soc_caps entry:
   the part, and the CPU frequency the build runs at. A software multiply is
   entirely CPU-bound, while the peripheral's cost is only partly so -- the rest
   runs at a clock that does not rise with the CPU. Raising the CPU frequency
   therefore makes software cheaper faster than it makes the peripheral cheaper,
   and pushes the crossover up: measured 348 -> 508 bits on the ESP32-S3 from 80
   to 240 MHz, while on the ESP32 the two effects nearly cancel (364 -> 374).

   Precision here buys nothing. Only a few operand sizes ever reach this
   function -- 256 and 384 (P-256 and P-384 ECC), 512, and RSA's 1024 and up --
   so every threshold in (384, 512] routes every real workload identically. What
   the value has to get right is which SIDE of 384 it falls on. The values below
   are rounded to that standard, not to the last measured bit.

   Under CONFIG_PM_ENABLE the CPU frequency varies at run time and this constant
   follows the configured maximum, so a DFS build sitting at a lower frequency
   uses a threshold slightly too high and sends a few multiplies to software
   that the hardware would have done marginally faster. Near the crossover the
   two costs are equal by definition, so the error is bounded by how far the
   frequency swings, and it is small. */
```

with the table itself as an explicit per-target `#if` chain on `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`, and a Kconfig escape hatch (`CONFIG_MBEDTLS_MPI_HW_MIN_BIT_LEN`, 0 meaning "use the built-in") so a user with an unusual clock or a measurement of their own is never stuck.

**Provenance has to be per-cell.** Only the ESP32 and ESP32-S3 rows are ours; every other row would be the reviewer's numbers, and the patch should say so in the comment rather than presenting nine measured parts as though one person measured them.

### Considered and rejected: calibrating the threshold at run time

Asked 2026-09-07: rather than a table, ship a default and have the port time a few multiplies on the real silicon at the real clock, then pick its own threshold. It is the obvious answer to "nobody has all nine parts", and it is wrong here for a specific reason rather than a general one about scope.

**The measurement it would perform is worse than the table it replaces.** Calibration has to run somewhere, and the only cheap moment is boot or first use. But the finding underneath this entire investigation is that EC maths costs **1.75× on core 0** because the WiFi and lwIP tasks deschedule it — the same handshake is 3.8 s on core 0 and 2.1 s on core 1. A calibrator in the boot path measures the scheduler, not the peripheral, in the noisiest window the system has. `spike/mbedtls-perf` exists precisely because the contended measurements were uninterpretable; re-introducing contention into the one number the routing depends on undoes that.

**Precision buys nothing anyway.** The threshold is not a continuous quantity in any workload — see the section above. Every value in (384, 512] routes every real operand identically. What the constant has to get right is one bit of information per (part, clock): *does 384 go to software?* Runtime probing spends real effort resolving a boundary whose exact position changes nothing.

**And the motivation has evaporated.** The case for self-calibration was that we have two parts and cannot measure the other seven. The review supplied nine parts across four clock points. The data problem is solved; what remains is how to encode it.

Three further objections, descending:

- **A crypto library that benchmarks itself is nondeterministic per boot.** Two boots of one image could route differently, making any performance report irreproducible. Not a constant-time violation — the choice depends on operand size, never on secret data — but a reviewer will ask, and the answer costs more goodwill than the feature is worth.
- **Caching the result to avoid recalibrating every boot adds NVS state**, and with it invalidation across firmware updates and frequency changes.
- **It changes the shape of the PR.** #19027 is a ~40-line routing fix that a reviewer has already reproduced. Bolting a self-benchmarking subsystem onto it turns it into an architecture proposal.

**The one variant worth keeping on a list**, as a follow-up and not in this PR: an opt-in `esp_mpi_calibrate_threshold()` behind a Kconfig defaulting off, called by the *application* when it knows the system is quiet, with the caller owning the result. That puts the "is it quiet enough to measure" judgement at the only layer that can make it and leaves the driver deterministic by default. The Kconfig override in the proposal above already covers most of that use case for a tenth of the surface.

**One thing the question did surface.** If the peripheral rounds operands up to a multiple of 16 words, then hardware cost below 512 bits is a *single number* per (part, clock), and the crossover falls out of two measurements instead of a sweep — which would make filling the per-part table cheap enough for Espressif to do on their board farm, removing the objection that made option C look expensive. Our ESP32 numbers are consistent with flat (19.49 / 23.40 / 22.15); our S3 numbers are clearly not (15.76 / 19.34 / 23.64). So the two parts do not round the same way, and that is question 3 of the run — it matters more than it first appeared.

### What the run answers

`spike/mbedtls-perf` cells `idf6-xover-{80,160,240}` and `idf6-s3-xover-{80,160,240}`, built 2026-09-07, not yet captured. Each is one binary carrying both curves — `SPIKE_MPI_THRESHOLD=0` sends every multiply to the peripheral for the hardware column, and `mul_soft()` in `main/bench.c` mirrors the port's software path for the software column — so the crossover is a within-binary comparison. That matters here more than usual: a 1424-byte code move has already cost this project 20–23% on the same source, so a hardware-versus-software crossover assembled from two different builds carries a layout difference of unknown size.

Three questions, in order of what they change:

1. **Does 384 go to software at 80 MHz on both parts?** If yes, the reviewer's low-clock rows overstate the case for hardware and the table's job is easy. If no, the S3 genuinely needs a frequency-dependent entry.
2. **Where does the ESP32 crossover actually sit** — nearer their 374 or our interpolated 408? Either way it is below 512 and the constant should come down.
3. **Is the hardware column flat from 256 to 512 on the ESP32?** The port rounds operands up to a multiple of 16 words before handing them over, so it should be. The S3's measured 15.76 → 19.34 → 23.64 is *not* flat, which means the two parts do not round the same way, and that is worth knowing before writing a shared formula.

## Methodology correction — cross-build percentages are not evidence (2026-09-10)

**Everything below this line that compares two separate firmware builds should be read with the ECC rows trusted and the `exp_mod` rows distrusted.** This document made the same mistake three times, and the mistake is one this project had already written down: *timing is a property of the binary, not the source* — a 1424-byte code move once cost 20–23% on unchanged source.

The wipe measurement is the clearest case. Comparing two builds gave +4.90% on RSA-2048 and +5.30% on RSA-4096, and a revert was pushed upstream on the strength of it. Re-measured with **both arms in one binary**, selected by a runtime flag so the two arms have identical code layout, and with a cycle counter around the wipe block so its cost is attributed rather than inferred:

| operation | op-level delta | cycles in the wipe block | explained | true cost |
|---|---|---|---|---|
| ECDSA verify P-256 | 741 µs | 741 µs | 100.1% | **0.32%** |
| ECDSA verify P-384 | 1127 µs | 1122 µs | 99.5% | **0.22%** |
| RSA-2048 public | 413 µs | 172 µs | 41.7% | **1.05%** |
| RSA-4096 public | 60 µs | 57 µs | 93.6% | **0.09%** |

Every arm was run twice later in the same sweep; the pairs agree to 0.01%. The wipe costs at most ~1%, not ~5%, and the revert was withdrawn (`05b269ea`).

**How to tell which rows to trust.** A per-call cost computed against the measured soft-path call count must be consistent across benchmarks, because it is the same code. The four ECC benchmarks agree to within 2% of each other. The `exp_mod` rows disagree with them by 27× and 87×, which is impossible for a fixed per-call cost and is the signature of layout contamination. Concretely, RSA-2048's op-level delta was **780 µs in one binary and 413 µs in another** that differed only by added instrumentation, while the cycles inside the wipe block stayed at 172–180 µs in both.

**Why `exp_mod` and not ECDSA.** The soft path runs 142 times inside a 16 ms hardware-bound RSA-2048 operation, so its direct cost is tiny next to the total and second-order effects dominate. ECDSA P-256 runs it 6330 times, so the direct cost swamps them.

**The stack-copy figures were re-measured too, and they were understatements.** Same method: a heap-copy arm and a stack-copy arm in one binary behind a runtime flag, both arms wiping (`mbedtls_mpi_free()` zeroizes, so the heap arm always did), so the delta is purely allocation against stack copy. A copy counter added on 2026-09-11 reports how many aliased operands each operation actually copies, incremented identically in both arms — which also asserts the two arms do the same work.

**Quote the per-copy cost, not the percentage.** The per-copy figure is what the change actually alters and it is stable across builds; the operation-level percentage depends on how often that operation copies *and* on code layout, and moves by about two points between binaries.

| operation | copies per call | **µs saved per copy** | words per copy | operation-level |
|---|---|---|---|---|
| ECDSA verify P-256 | 1486 / 6330 = 0.23 | **14.7** | 7.9 | ≈8% |
| ECDSA verify P-384 | 2139 / 9359 = 0.23 | **14.8** | 11.8 | ≈5.8% |
| RSA-2048 public | 142 / 142 = 1.00 | **20–23** | 2.0 | ≈14–16% |
| RSA-4096 public | 173 / 173 = 1.00 | **21.7** | 2.0 | ≈5.8% |

Earlier cross-build figures said −7.4% on ECDSA P-256 and −8.9% on RSA-2048; RSA-2048 is nearer 14–16% and RSA-4096 moves from a rounding error to ≈5.8%. The reviewer's patch is worth more than either of us measured.

**The per-call gap between ECC and RSA was copy frequency, and that is now measured rather than inferred.** ECDSA reaches the soft path 6330 times but only 1486 of those alias the destination; RSA copies on every single call. Per copy the two families are within 1.5× of each other. The counts are identical in both arms of every row.

**RSA copies are two words.** The heap arm spends about 21 µs to allocate, copy and free eight bytes. That is allocator overhead almost in its entirety — the effect the reviewer identified — and about five times their ~4.4 µs estimate.

**The consistency check passes.** RSA-2048 and RSA-4096 agree at 20–23 µs per copy where across builds they disagreed by 3.6×; P-256 and P-384 agree at 14.7 and 14.8.

**The rule this leaves.** Measure A against B inside one image, or do not claim a percentage. The crossover cells and the mul-step sweep already worked this way, which is why those results held up.

## MEASURED — 14 cells, both boards, 2026-09-07

All captures in `spike/mbedtls-perf/results/`, collated in `results.csv`. Every cell asserted its SDK patch state before *and* after the flash, its board by MAC, and its target and CPU frequency out of the generated `sdkconfig`; `[id]` carries target, configured and running clock, and the routing threshold, so no row depends on its filename.

### 1. The stack copy the review asked for: adopt it

`-fix` is #19027 as submitted (aliased operand copied into a heap `mbedtls_mpi`); `-fix-stackcopy` adds the reviewer's patch (the same copy on the stack, bounded by the routing threshold).

| | ESP32 | ESP32-S3 |
|---|---|---|
| ECDSA verify P-256 | 250.74 → 232.25 ms — **−7.4%** | 199.28 → 187.34 ms — **−6.0%** |
| ECP mul P-256 | 117.55 → 108.11 ms — −8.0% | 92.89 → 86.55 ms — −6.8% |
| ECDSA verify P-384 | 526.72 → 500.90 ms — −4.9% | 412.07 → 394.85 ms — −4.2% |
| **RSA-2048 public** (`exp_mod`, e=65537) | ~~−8.9%~~ → **≈14–16%**, or **20–23 µs per copy** | ~~−4.0%~~ ESP32-S3 not re-measured | **← re-measured within a binary; quote the per-copy figure** |
| `mpi_mul` 256 | 10.30 → 10.02 µs — −2.7% | 7.76 → 7.54 µs — −2.9% |
| `mpi_mul` 512 / 1024 / 2048 / 4096 | +0.06 / +0.06 / +0.02 / +0.20% | +0.01 / −0.00 / −0.00 / +0.02% |

**The last row is the control that licenses the rest.** Operands of 512 bits and up route to the peripheral and never enter the patched function; they move by less than 0.2% on the ESP32 and less than 0.03% on the S3. The improvements above are therefore the patch, not build noise or layout.

The reviewer's diagnosis was right: the heap allocation costs more than the multiply it protects, and it was making RSA-2048 public operations *slower* than unpatched. Their fix removes it. No row regresses on either part.

### 2. The crossover disagreement was an artefact of our own patched tree

The six `*-xover-*` cells plus two `-ct-out` controls settle it. The controls are identical builds with the mbedTLS submodule reverted, so the TF-PSA-Crypto #873 constant-time series is out — and they report `idf=v6.0.2` rather than `v6.0.2-dirty`, which is independent confirmation that the tree really was clean.

**The hardware column is bit-for-bit identical between the two ESP32 builds** — 20.36 / 20.62 / 20.85 / 21.09 / 21.34 / 21.57 / 21.81 / 22.06 / 22.30 µs across 256→512 in both. Only the software path moved. That is as clean a control as this harness can produce.

| ESP32 @ 240 MHz | crossover | 384 bits |
|---|---|---|
| stock mbedTLS (`-ct-out`) | **~383** | hardware, by 0.4% |
| with #873 (what smolbase ships) | **~402** | software, by 7.8% |
| the reviewer's figure | 374 | hardware |

**Our clean-tree 383 against their 374 is 2.4% apart.** There was no disagreement about the ESP32 crossover. We were measuring a patched mbedTLS and they were not. The #873 series makes the software multiply 6–12% faster, which lifts the crossover by about 5% — exactly the offset observed, at every clock: ours 394/397/402 against their 364/369/374, with the same near-flat frequency slope (+8 bits across a 3× clock change, against their +10).

**This is the finding with a consequence.** 384 bits flips sides. On stock mbedTLS it is a dead heat there; with #873 in, software wins by 7.8%. **#873 is approved and merging, so any threshold constant chosen against today's mbedTLS is wrong for P-384 operands once it lands** — and P-384 is not hypothetical, it is GitHub's root.

### 3. 384 bits goes to software in every cell we ship

| board | clock | crossover | 384 goes to |
|---|---|---|---|
| ESP32 | 80 MHz | ~394 | software by 4.6% |
| ESP32 | 160 MHz | ~397 | software by 5.8% |
| ESP32 | 240 MHz | ~402 | software by 7.8% |
| ESP32-S3 | 80 MHz | ~398 | software by 5.2% |
| ESP32-S3 | 160 MHz | ~440 | software by 16.9% |
| ESP32-S3 | 240 MHz | above 512 | software by 27.5% |

On the S3 the answer holds even without #873: 384 → software by 19.1% on the clean tree. On the ESP32 without #873 it is a 0.4% dead heat, which is to say it does not matter either way there.

So the shipped 512 is safe at every operand size that occurs, on both parts, at every clock — and the reviewer's 374 (ESP32) and 348 (S3 at 80 MHz) would both route 384 to hardware, which our data says is the slower choice on the tree we ship.

### 4. A tooth can masquerade as a crossover

The S3 clean-tree cell reports a sign change at ~464 bits and then software winning again at 512. That first flip is not a crossing, it is the 480-bit tooth described in the next section. A table built by sweeping upward and taking "the first size where hardware wins" will land on a tooth rather than the true crossing — and the reviewer's S3-at-240 figure of 508 sits exactly where a tooth-driven answer would.

`run.ps1`'s crossover summary now detects this and says so rather than reporting the tooth as the answer.

## The software multiply is a sawtooth in limb count, not a smooth curve (2026-09-07)

The 32-bit-step sweep turned up something the 256/384/512 sweep could never have shown: **480 bits costs more than 512 bits**, in software, in every one of the six crossover cells. On the ESP32-S3 at 240 MHz, 21.50 µs against 20.33 µs — 512 bits does strictly more work and takes 5.4% less time.

It is not noise and not a measurement artefact. It is `mbedtls_mpi_core_mla()`, which every software multiply goes through:

```c
size_t steps_x8 = s_len / 8;
size_t steps_x1 = s_len & 7;

while (steps_x8--) { MULADDC_X8_INIT MULADDC_X8_CORE MULADDC_X8_STOP }
while (steps_x1--) { MULADDC_X1_INIT MULADDC_X1_CORE MULADDC_X1_STOP }
```

The inner loop is unrolled by eight, and the remainder is done one limb at a time. On Xtensa there is no architecture-specific `MULADDC_X8`, so `bn_mul.h`'s generic fallback expands it to eight inlined copies of the X1 core sharing **one** `INIT` and one `STOP`. And `MULADDC_X1_INIT` is not free — it opens a scope, declares six locals, and splits the multiplier into half-words:

```c
#define MULADDC_X1_INIT                 \
{                                       \
    mbedtls_mpi_uint s0, s1, b0, b1;    \
    mbedtls_mpi_uint r0, r1, rx, ry;    \
    b0 = ( b << biH ) >> biH;           \
    b1 = ( b >> biH );
```

So the remainder loop redoes that setup **per limb** where the unrolled block does it once per eight. A 15-limb operand pays one 8-block plus seven of those; a 16-limb operand pays two 8-blocks and nothing else. The outer loop in `mbedtls_mpi_core_mul()` runs once per limb of the other operand, so the penalty is multiplied by the operand size again.

### The model

Cost of a square multiply of *n* limbs, with the outer loop running *n* times:

```
cost(n) = o + n * ( floor(n/8) * C8 + (n mod 8) * C1 )
```

Three constants, fitted on the ESP32-S3 at 240 MHz from three points — *n*=8 and *n*=16 (the only sizes with no remainder at all) give `o` and `C8`, then *n*=15 gives `C1`:

| | |
|---|---|
| fixed overhead `o` | 2.703 µs |
| one 8-limb block `C8` | 0.5508 µs — **0.0689 µs/limb** |
| one leftover limb `C1` | 0.1003 µs — **1.46× the unrolled rate** |

It then predicts the other six measured points to within 1.3%:

| limbs | bits | predicted | measured | error |
|---|---|---|---|---|
| 8 | 256 | 7.11 | 7.11 | fitted |
| 9 | 288 | 8.56 | 8.63 | −0.8% |
| 10 | 320 | 10.22 | 10.35 | −1.3% |
| 11 | 352 | 12.07 | 12.22 | −1.2% |
| 12 | 384 | 14.13 | 14.29 | −1.1% |
| 13 | 416 | 16.39 | 16.51 | −0.8% |
| 14 | 448 | 18.84 | 18.93 | −0.5% |
| 15 | 480 | 21.50 | 21.50 | fitted |
| 16 | 512 | 20.33 | 20.33 | fitted |

As a cross-check it was never fitted against: the 2026-08-30 S3 software figure at 1024 bits (32 limbs, no remainder) was 75.78 µs; this model says 73.2 µs, 3.4% out across a different build and a different measurement method.

**A leftover limb costs 1.46× what an unrolled one does.** That is the whole effect, and it is generic C — nothing to do with the ESP32's peripheral, and present on any target without an assembly `MULADDC_X8`.

### Why it matters here

**It does not change any routing decision.** The teeth sit at 15 limbs (480 bits) and 23 (736); the crossover on the ESP32 is ~400 bits and the operand sizes that actually occur are 256, 384, 512 and RSA's 1024+ — all either well clear of a tooth or exactly on a multiple of 8 limbs. P-256 is 8 limbs and P-384 is 12; RSA-1024/2048/4096 are 32/64/128. The one common size with a remainder is P-384's 12 limbs, which pays four leftover limbs per outer iteration and is ~1.1% off the smooth trend — real, but nothing anyone would chase.

**It does change how the threshold table should be derived.** The software curve is not `k·n²`; it is `k·n²` plus a sawtooth of up to 1.46× on the remainder limbs. Anyone fitting a smooth function to two or three measured sizes and extrapolating a crossover will be wrong by an amount that depends on which side of a tooth their sample points landed. That is an argument for measuring at the operand sizes that matter rather than fitting a curve — and it is one more reason the threshold only needs to classify 384 correctly rather than name a precise bit count.

**It is also a real, if small, upstream finding in its own right.** `mbedtls_mpi_core_mla()` could handle the remainder with the X4/X2 blocks that `bn_mul.h` already defines — 7 leftover limbs as X4+X2+X1 rather than seven X1s — which would cut the tooth by most of its height for any size not a multiple of 8. Worth raising separately from #19027; it is a different file, a different argument, and mixing them would help neither.

### Verification — predicted before measuring, then confirmed on both parts

The model was written down from the source and fitted to the 256..512 sweep, and it predicted the tooth must repeat one octave up: **23 limbs (736 bits) costing more than 24 (768)**. Cells `idf6-mulstep-240` and `idf6-s3-mulstep-240` then swept 480 to 800 bits in 32-bit steps. Both found it:

| | 23 limbs (736 bits) | 24 limbs (768 bits) | |
|---|---|---|---|
| ESP32 @ 240 | 61.73 µs | 59.58 µs | the larger operand is **3.6% cheaper** |
| ESP32-S3 @ 240 | 42.90 µs | 40.91 µs | **4.9% cheaper** |

Refitting the same three constants on the ESP32 mul-step data alone — using only the two remainder-free sizes (16 and 24 limbs) plus one remainder size (23) — predicts the other eight points to within **1.3%**:

| limbs | bits | predicted | measured | |
|---|---|---|---|---|
| 15 | 480 | 30.42 | 30.03 | tooth |
| 16 | 512 | 29.05 | 29.05 | fitted |
| 17 | 544 | 32.90 | 32.65 | |
| 18 | 576 | 37.02 | 36.91 | |
| 19 | 608 | 41.42 | 41.46 | |
| 20 | 640 | 46.08 | 45.97 | |
| 21 | 672 | 51.03 | 50.60 | |
| 22 | 704 | 56.24 | 56.19 | |
| 23 | 736 | 61.73 | 61.73 | fitted, tooth |
| 24 | 768 | 59.58 | 59.58 | fitted |
| 25 | 800 | 65.29 | 65.27 | |

And the constant that matters — the cost of a leftover limb relative to an unrolled one — comes out at **1.43× on the ESP32 and 1.46× on the ESP32-S3** from two entirely independent fits on two different parts. The mechanism is settled.

**It also produced a false crossover, which is the practical warning.** The S3 clean-tree cell shows software winning at every size except 480 bits, where the tooth lifts it just above the hardware curve. A sweep that stops at the first size where hardware wins would report ~464–480 there and call it the crossover — and the reviewer's S3-at-240 figure of 508 sits exactly where a tooth-driven answer would land. `run.ps1`'s crossover summary now checks whether software wins again above the first sign change and says "that flip is a TOOTH, not a crossing" when it does.

## Carried forward from the review correspondence (2026-09-14)

The reply drafts that carried these points have been deleted from the repository; the findings in them are recorded here, which is where they belonged in the first place. The PR thread itself is the record of what was sent.

## How the threshold is actually applied, and why 385 rather than 384

The routing test is in `mbedtls_mpi_mul_mpi()`:

```c
size_t x_bits = mbedtls_mpi_bitlen(X);
size_t y_bits = mbedtls_mpi_bitlen(Y);
...
if (MAX(x_bits, y_bits) < SOC_MPI_HW_MIN_BIT_LEN) {
    return mpi_mult_mpi_soft(Z, X, Y);
}
```

Three properties follow, and all three matter when choosing the constant.

**It compares against `MAX(bitlen(X), bitlen(Y))`** — the larger of the two operands.

**`bitlen` is a property of the value, not of the type.** It is the position of the highest set bit of the number actually held. Not the allocated limb count, not the curve size. A P-384 field element holding the value 5 has `bitlen` 3 and routes to software whatever the threshold is.

**The test is strictly less than.** So a threshold of 385 means "larger operand of 384 bits or fewer goes to software". 385 is the smallest value that puts every P-384 field element on the software side; at 384, an operand whose top bit sits at position 384 goes to hardware while one at 383 goes to software.

Because the test is per value, "P-384 goes to software" is statistical rather than exact. `p384` is just under 2^384, so a uniformly distributed field element has `bitlen` 384 about half the time, 383 a quarter of the time, and so on. The chance of falling below 374 — the reviewer's ESP32 value — is under 2^-10, so **roughly 99.9% of P-384 operands land on whichever side 384 does.** Treating it as binary is a fair description of what happens.

The same reasoning explains an otherwise puzzling measurement: the copies inside an RSA-2048 `exp_mod` are **two words, 64 bits**. Those are not 2048-bit operands. They are small intermediates of the modular arithmetic whose `bitlen` happens to be tiny, so they route to software and reach `mpi_mult_mpi_soft()` — which is why a 2048-bit operation touches the soft path 142 times and copies on nearly every one.

Note also that the port already carried an **upper** bound before this work, at the same call site: `if (hw_words * 32 > SOC_RSA_MAX_BIT_LEN/2)` falls back for operands too large for the peripheral. The new constant is the matching lower bound.

## The CT change is not uniform across architectures

[Mbed-TLS/TF-PSA-Crypto#873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873) is three patches, and only two of them are generic:

| patch | file | scope |
|---|---|---|
| `0001` force inlining, the `-Os` loss | `constant_time_impl.h` | no arch guard — **generic** |
| `0002` assembly path | `constant_time_impl.h` | `__XTENSA__` — **Xtensa only** |
| `0003` `_if_else_0` | `bignum_core.c` | no arch guard — **generic** |

So the ESP32, ESP32-S2 and ESP32-S3 receive all three. The RISC-V parts receive the two generic patches only. **Our +5% crossover shift was measured on an ESP32 with all three and must not be extrapolated to RISC-V.**

**The `S` prefix does not imply Xtensa.** The S2 and S3 are Xtensa; the **ESP32-S31 is a dual-core RISC-V part**. Classify by architecture, not by name.

## Choosing a constant that survives the CT change

The CT change moves the ESP32 crossover from 383 bits to 402. The only operand size between those is 384, so **any threshold from 385 to 512 classifies every size that occurs identically before and after it** — 256 and 384 for elliptic curves, 512, and RSA's 1024 and above.

The choice at 384 is asymmetric, measured on the ESP32 at 240 MHz:

| threshold | 384 goes to | without the CT change | after it |
|---|---|---|---|
| 384 or less | hardware | 0.4% better | **7.8% worse** |
| 385 or more | software | 0.4% worse | **7.8% better** |

Paying 0.4% now to avoid 7.8% later is the easy side of that trade, and P-384 is not hypothetical — it is GitHub's root.

Applied to the reviewer's whole table, using for each part the clock whose crossover is **nearest 384**, because that is the hardest case for the claim:

| target | arch | clock | value | with 0–5% more | 384 goes to | distance from 384 | closest after CT |
|---|---|---|---|---|---|---|---|
| esp32 | Xtensa | 240 MHz | 374 | 374–393 | **changes side** | 10 | 0 |
| esp32s2 | Xtensa | 240 MHz | 325 | 325–341 | hardware | 59 | 43 |
| esp32s3 | Xtensa | 160 MHz | 399 | 399–419 | software | 15 | 15 |
| esp32c3 | RISC-V | 160 MHz | 269 | 269–282 | hardware | 115 | 102 |
| esp32c5 | RISC-V | 160 MHz | 356 | 356–374 | hardware | 28 | 10 |
| esp32c6 | RISC-V | 160 MHz | 389 | 389–408 | software | 5 | 5 |
| esp32s31 | RISC-V | 240 MHz | 556 | 556–584 | software | 172 | 172 |
| esp32h2 | RISC-V | 64 MHz | 346 | 346–363 | hardware | 38 | 21 |
| esp32p4 | RISC-V | 400 MHz | 603 | 603–633 | software | 219 | 219 |

**Only the esp32 row changes side for any shift between 0% and 5%**, so the argument holds without knowing the RISC-V figure.

The last two columns are different risks and should not be conflated. *Distance from 384* is how near the measured value is, so how much error in the measurement would flip 384. *Closest after CT* is how near the 0–5% band comes, so whether the CT change alone can flip it. The **C6 has 5 bits** of the former and the **C5 has 28** (10 at the top of its band); for both, the risk is the measurement rather than the CT change, and both would be worth re-measuring.

Every operand size that occurs is a whole number of 32-bit limbs — 256, 384, 512 — so no other operand size falls in these gaps. Only 384 is at stake.

## The sawtooth does not need code

The peaks at 15 limbs (480 bits) and 23 limbs (736 bits) do not create several crossovers. They create **one false sign change** if a sweep goes upward and stops at the first size where hardware wins. The remedy is to measure at the operand sizes that occur rather than to sweep; nothing in the port needs to change. The reviewer's ESP32-S3 value at 240 MHz (508) sits close to where such a sweep would stop on the 480-bit peak.

## Prior art

[espressif/esp-idf#8710](https://github.com/espressif/esp-idf/issues/8710) reports the same symptom — a TLS client tripping the task watchdog, fixed by disabling hardware MPI — with no measurement and no mechanism, and it is closed. No report was found that quantifies it or identifies the cache component, but that was a keyword search, so treat "not reported" as weak.

## What is still not established

- **The cache component is measured on the ESP32 (LX6) only.** An S3 was available for other parts of this work, but the flash-clock test was never run on it. The RISC-V parts are untested.
- **No profiling of which code evicts what.** The conclusion rests on the flash-clock test and on the tight-loop-versus-in-situ split, not on cache performance counters.
- **Everything here is `-Os`.** `-O2` was tried early and made TLS *slower*, which is consistent with the same mechanism, but it was not investigated.
- **The ESP32-S3 stack-copy figure was never re-measured within a binary.** Only the ESP32 was.
