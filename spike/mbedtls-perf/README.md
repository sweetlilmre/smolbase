# mbedTLS perf spike

The harness for [docs/research/mbedtls4-perf-spike.md](../../docs/research/mbedtls4-perf-spike.md). It measures individual mbedTLS primitives on the device, offline, in both mbedTLS 3.6.6 and 4.1.0, with the ESP32's RSA/MPI accelerator on and off.

It exists because the migration produced a working TLS fix whose *reason* is not established. The end-to-end numbers are solid; the mechanism is inference, one part of it already falsified. This measures the mechanism directly.

**It has been run.** All twelve cells are captured in `results/`. **C2 refuted** — both versions make exactly 5817 `mul_mpi` calls per P-256 verify. **C3 confirmed** — 19.49 µs hardware against 9.78 µs software at 256 bits, with the crossover between 256 and 512 bits. **The missing cell says the accelerator was always wrong**: mbedTLS 3.6.6 is 17.8% faster without it too. Full write-up in the [research doc](../../docs/research/mbedtls4-perf-spike.md), which is now the single record and handover for this work.

## What it answers

**C2 — does mbedTLS 4's ECP call `mbedtls_mpi_mul_mpi` more often per verify than 3.6.6?** `mul_calls_per_op` on the `ecdsa_verify_p256` row, in both versions. The claim currently written into `sdkconfig.defaults` says yes; nobody has ever counted.

**C3 — at ECC operand sizes, does the accelerator's per-call ceremony cost more than a software multiply?** The `mpi_mul` sweep from 256 to 4096 bits, run with the accelerator on and off. The ESP port applies no lower size threshold, so if C3 holds there is a crossover size below which the peripheral is a pure loss — and 256 bits is below it.

**The missing cell.** mbedTLS 3.6.6 with the accelerator *off* was unmeasurable during the migration, because the arduino-esp32 mbedTLS ships precompiled. It is the highest-value number here: if 3.6.6 is also much faster without the accelerator, then the accelerator was always wrong for ECDSA on this chip and explains none of the version gap.

## Confounds it removes by construction

The original measurements were wall-clock HTTPS requests to a live third-party host from the full firmware. This app has no WiFi, no TLS, no HTTP, no JSON, no display and — importantly — no 200 Hz touch filter on core 0, which accounted for three of eight stack samples in the original profiling. Fixed operands, a fixed private key, one task pinned to core 1, `esp_timer` as the clock.

What it does *not* remove: it measures primitives, not a handshake. A result here explains the handshake; it does not reproduce it.

## Building

The same source builds unchanged under both SDKs. Whichever `export.ps1` is sourced picks the mbedTLS version — there is nothing to switch in the tree.

| | | |
|---|---|---|
| ESP-IDF v6.0.2 | mbedTLS 4.1.0 (TF-PSA-Crypto) | `& $HOME\esp\esp-idf\export.ps1` |
| ESP-IDF v5.5.5 | mbedTLS 3.6.6 | `& $HOME\esp\esp-idf-v5.5\export.ps1` |

```
& $HOME\esp\esp-idf\export.ps1
idf.py '@idf6-mpi-on.args' build      # also @idf6-mpi-off, -on-fp, -off-fp

& $HOME\esp\esp-idf-v5.5\export.ps1
idf.py '@idf5-mpi-on.args' build      # and the three idf5-* variants
```

Eight argfiles, one per (SDK × accelerator × fixed-point) cell, plus four `*-nowrap` cells built with `-D SPIKE_NO_WRAP=1` — the uninstrumented control, which omits the `-Wl,--wrap` link options and compiles the hooks out entirely. That control exists because the hook's cost had to be established independently of the hook, and it is what proved the `mpi_mul_hooked` margin above to be an artefact. Its captures report `wrap=0` in `[id]` and zero for every call count, by construction.

Each argfile carries **its own build directory and its own generated `sdkconfig`**. That is deliberate: `sdkconfig.defaults` only supplies values *absent* from an existing generated `sdkconfig`, so sharing a build directory between configurations silently measures the first one four times. Separate directories cost a cold build each and cannot go wrong.

Quote the `@` in PowerShell — bare `@` is the splatting operator.

## The #19027 review run — captured 2026-09-07

**Result: adopt the stack copy.** ECDSA P-256 verify −7.4% on the ESP32 and −6.0% on the S3; the RSA-2048 public operation the reviewer said had regressed, −8.9% and −4.0%. Operands of 512 bits and up — which never enter the patched function — move by under 0.2%, which is the control that makes the rest believable. Full numbers in [espressif-mpi-threshold-open-question.md](../../docs/research/espressif-mpi-threshold-open-question.md).

The [espressif/esp-idf#19027](https://github.com/espressif/esp-idf/pull/19027) review on 2026-09-04 asked for one change and disputed one number. These four cells are the harness for the change; the disputed number gets its own six in *The crossover cells* below.

**The change.** The threshold patch routes sub-threshold multiplies to a software path that copies an aliased operand into a heap `mbedtls_mpi`. The reviewer measured that heap allocation costing ~4.4 µs per call — more than the multiply — which makes an RSA-2048 public operation *slower* than unpatched, because inside one every small multiply aliases `Z` with an operand. Their patch bounds the copy by the routing threshold and puts it on the stack instead. They report both ECDSA and RSA improving.

**Four cells, two boards, one variable.** `-fix` is the threshold patch as submitted (heap copy), `-fix-stackcopy` is that plus the reviewer's patch:

| tag | board | SDK bignum port |
|---|---|---|
| `idf6-mpi-on-fix` | ESP32, COM5 | `esp-idf-mpi-min-bitlen.patch` |
| `idf6-mpi-on-fix-stackcopy` | ESP32, COM5 | + `esp-idf-mpi-stack-copy.patch` |
| `idf6-s3-mpi-on-fix` | ESP32-S3, COM9 | `esp-idf-mpi-min-bitlen.patch` |
| `idf6-s3-mpi-on-fix-stackcopy` | ESP32-S3, COM9 | + `esp-idf-mpi-stack-copy.patch` |

```
& $HOME\esp\esp-idf\export.ps1
./run.ps1 -Runs idf6-mpi-on-fix,idf6-mpi-on-fix-stackcopy -Flash                 # ESP32
./run.ps1 -Runs idf6-s3-mpi-on-fix,idf6-s3-mpi-on-fix-stackcopy -Flash           # ESP32-S3
```

`run.ps1` picks the port from the tag, so the two commands above need no `-Port`.

**Accelerator-on only, and that is not an omission.** `components/mbedtls/CMakeLists.txt` compiles `esp_bignum.c` only `if(CONFIG_MBEDTLS_HARDWARE_MPI)`. With the accelerator off the patched file is not in the build at all, so an `mpi-off` cell would measure nothing and invite the reader to think it had.

**The rows that answer the review** are `mpi_exp_mod_e65537` at 2048 bits — that is the RSA public operation the reviewer says regressed, `e=65537` against a 2048-bit modulus — and `ecdsa_verify_p256`. The `mpi_mul` sweep is the mechanism underneath both. Every cell is `SPIKE_NO_WRAP=1`: the hook costs 0.21–1.30 µs per call and an ECDSA verify makes thousands, which would swamp a 4.4 µs-per-call effect.

### What is being measured lives in the SDK tree, not here

Nothing in this directory records which variant a build used, because the patched file is `$IDF_PATH/components/mbedtls/port/bignum/esp_bignum.c`. That is the same shape as the failure this README already warns about — a patched cell built, the patch reverted, then `idf.py flash` recompiling it unpatched during the flash step and capturing that. `sdk_variant.ps1` exists so nobody switches the tree by hand:

```
./sdk_variant.ps1 -Verify                 # what is in the tree right now
./sdk_variant.ps1 -Set fix                # threshold patch only, heap copy
./sdk_variant.ps1 -Set fix-stackcopy      # + the reviewer's patch
./sdk_variant.ps1 -Set fix-stackcopy-wipe # + the zeroize fix (3 of the firmware's 4)
./sdk_variant.ps1 -Set stock              # pristine v6.0.2
```

`run.ps1` calls it from the tag immediately before the flash — which is the moment that matters, because flashing rebuilds — and then reads the tree back and asserts. It also reads the chip id read-only and refuses to flash a board whose MAC is not the one the tag expects. Both ESP32-S3 boards on the bench are ours as of 2026-09-28, but the MAC check earns its keep anyway: COM numbers move between sessions, so a remembered port is not an identification.

**Leave the tree in `stock` when you are done** — `./sdk_variant.ps1 -Set stock`. The firmware ships FOUR patches and this script applies only the first three, so every variant it can produce, `fix-stackcopy-wipe` included, leaves the series partially applied: the first patch no longer applies and the last does not reverse-apply, which is exactly the state `cmake/upstream_patches.cmake` FATALs on. That is the design working, but it means there is no variant you can leave behind. From `stock` the first patch applies cleanly and cmake lays down all four.

### Two things to read the patch for, before the numbers

- **`MPI_SOFT_MAX_WORDS` divides by a hardcoded 32.** The bound itself is right — the caller routes on `MAX(x_bits, y_bits) < SOC_MPI_HW_MIN_BIT_LEN`, so each operand has at most `ceil((threshold-1)/32)` significant words and the rounding up covers a threshold that is not a multiple of 32. But 32 is the width of `mbedtls_mpi_uint` on these 32-bit parts by assumption, not by construction; `sizeof(mbedtls_mpi_uint) * 8` says the same thing and stays true if the port is ever built where `MBEDTLS_HAVE_INT64` holds. Worth raising on the PR rather than measuring.
- **The aliasing analysis is sound.** `xp`/`yp` are captured before `mbedtls_mpi_grow(Z, …)` can reallocate `Z->p`, and an aliased operand has already been copied out by then; `z_sign` is read before `Z` is written. `string.h` is already included in `esp_bignum.c`.

## The crossover cells — captured 2026-09-07

**Result: the disagreement was ours.** We had been measuring an mbedTLS carrying the TF-PSA-Crypto #873 series, which makes the software multiply 6–12% faster and lifts the crossover ~5%. On a clean tree the ESP32 gives ~383 bits against the reviewer's 374 — agreement. The consequence is that **#873 flips 384-bit operands from the hardware side to the software side**, so a constant chosen against today's mbedTLS is wrong once it reaches the SDK — #873 merged upstream on 2026-10-28, and these cells are what the re-measure in [#134](https://github.com/sweetlilmre/smolbase/issues/134) will re-run. 384 routes to software in all six shipped-tree cells.

The other half of the same review. Their table puts the ESP32 crossover at 364–374 bits and the ESP32-S3 at 348 (80 MHz) → 508 (240 MHz), against the flat 512 in the patch. The S3-at-240 figure agrees with this project's own measurement; the rest does not, and the constant clearly is not one number.

**The only part of the disagreement that changes a routing decision is 384 bits.** Only a few operand sizes ever reach `mbedtls_mpi_mul_mpi` — 256 and 384 (P-256 and P-384 ECC), 512, and RSA's 1024 and up — so every threshold in (384, 512] routes every real workload identically, and 374 versus our interpolated ~408 is a distinction without a difference. What matters is which side of **384** the threshold falls on, because GitHub's chain has a P-384 root. This project measured software winning at 384 on both parts (7% on ESP32, 18% on S3); their ESP32 figure of 374, and their S3 figure of 348 at 80 MHz, both put 384 on the hardware side. Full argument in [espressif-mpi-threshold-open-question.md](../../docs/research/espressif-mpi-threshold-open-question.md).

Six cells, 32-bit steps from 256 to 512, three CPU frequencies per board:

| tag | board | CPU |
|---|---|---|
| `idf6-xover-240` / `-160` / `-80` | ESP32, COM5 | 240 / 160 / 80 MHz |
| `idf6-s3-xover-240` / `-160` / `-80` | ESP32-S3, COM9 | 240 / 160 / 80 MHz |

```
& $HOME\esp\esp-idf\export.ps1
./run.ps1 -Runs idf6-xover-240,idf6-xover-160,idf6-xover-80 -Flash
./run.ps1 -Runs idf6-s3-xover-240,idf6-s3-xover-160,idf6-s3-xover-80 -Flash
```

**Both curves come out of one binary**, which is the whole design. `SPIKE_MPI_THRESHOLD=0` disables the routing so every multiply reaches the peripheral — that is the `mpi_mul_hw` column — while `mul_soft()` in `main/bench.c` mirrors the port's software path for `mpi_mul_sw`. They are interleaved size by size in `BENCHES` so a thermal or scheduler drift over the run cannot masquerade as a crossover. Taking the two columns from two builds instead would carry a layout difference of unknown size: a 1424-byte code move has already cost this project 20–23% on identical source.

These cells run against the **stock** SDK tree, not a patched one. With the threshold at zero the patch would change nothing, and one less moving part is one less thing to be wrong about; `run.ps1` sets and asserts that state like it does for the others.

The frequency is a Kconfig **choice**, so `sdkconfig.cpu80`/`sdkconfig.cpu160` unset the 240 pick explicitly — adding the alternative alone leaves the earlier `=y` winning, silently. `run.ps1` asserts the generated `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` against the tag, and `[id]` now reports `cpu_mhz` (configured), `cpu_mhz_rt` (actually running), `target` and `mpi_min_bits` so a capture says what it measured without reference to its filename.

### Reading them

`mpi_mul_hw` minus `mpi_mul_sw` at each size; the crossover is the sign change. Three things to look for:

1. **Which side of 384 the crossing falls on, at every frequency.** That is the answer to the review.
2. **How far the crossover moves with frequency**, per part. Their ESP32 barely moves (364 → 374) and their S3 moves 46% (348 → 508); the mechanism is that a software multiply is entirely CPU-bound while the peripheral's cost is only partly so, and the ratio differs per part.
3. **Whether the hardware column is flat from 256 to 512.** On the ESP32 it should be — the port rounds operands up to a multiple of 16 words before handing them over. The S3's measured 15.76 → 19.34 → 23.64 is not flat, so the two parts evidently do not round the same way, which matters before anyone writes a shared formula.

## Running

**Flashing this spike erases the smolbase firmware.** It writes its own bootloader and its own default `single_app` partition table, so the device's custom layout from `partitions.csv` goes with it, and the NVS holding the WiFi credentials with that. Getting the device back is a full serial flash of the real firmware plus a re-provision through the captive portal. `run.ps1` therefore does nothing destructive without `-Flash`.

```
./run.ps1                      # check every build and its generated sdkconfig, flash nothing
./run.ps1 -Runs idf6-mpi-on -Flash
./run.ps1 -ParseOnly           # collate results/*.log into results/results.csv
```

**`idf.py flash` rebuilds, so the source tree must be in the intended state at FLASH time, not just at build time.** This produced a silently wrong result once: a patched cell was built, the patch was then reverted, and `run.ps1` recompiled it unpatched during the flash step and captured that. The numbers came back byte-identical to the unpatched run, which is the only reason it was caught. Always disassemble the flashed ELF afterwards and confirm the change is actually in it.

`run.ps1` refuses a run whose tag does not match the active `IDF_PATH`, and asserts `CONFIG_MBEDTLS_HARDWARE_MPI` and `CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM` in the *generated* `sdkconfig` before flashing. Both checks are there because a Kconfig value that failed to take reads as a perfectly plausible measurement of the wrong thing, which has already cost this project a day.

Serial is COM5 with RTS auto-reset wiring. The capture loop uses a 512 KB receive buffer and does nothing but `ReadExisting()` — a default `SerialPort` drops bytes during the boot burst and produces plausible-but-wrong text.

## Reading the output

```
[id] mbedtls=4.1.0 idf=v6.0.2 hw_mul=1 hw_exp=1 fixed_point=0 cpu_mhz=240 opt=mbedtls-Os core=1 free_internal=...
[vec] d=... sig_r=... sig_s=...
[ok] wrap active (4271 mul calls during the verify bench)
[bench] name=ecdsa_verify_p256 bits=256 iters=8 reps=5 min_ns=... mean_ns=... max_ns=... mul_calls_per_op=... ...
```

- **`[id]` is the ground truth for what was measured.** `hw_mul` and `fixed_point` are read from the compiled macros, not from the argfile name.
- **`[vec]`'s `sig_r`/`sig_s` must match between the 3.6.6 and 4.1.0 runs.** RFC 6979 signing makes the signature a function of key and hash alone, so a difference means the two versions are not verifying the same thing and the verify rows are not comparable.
- **`[ok] wrap active` must appear.** A `-Wl,--wrap` that resolves nothing reports zero calls, which is indistinguishable from "this version never calls it". The check is on the ECDSA benchmark, which provably calls the hooked symbol.
- **`mul_calls_per_op` and `exp_calls_per_op` are scaled by 1000** — `4271` means 4.271 calls per operation. They are integers because newlib-nano's `printf` silently drops `%f`.
- **`mpi_mul_hooked` does NOT price the instrumentation, despite what this README used to say.** Its margin over `mpi_mul`/256 is ~9.8 µs with the accelerator on and ~1.2 µs with it off — and a `SPIKE_NO_WRAP` build, which links no hook at all, reproduces exactly the same margin. It is a benchmark-ordering artefact: `mpi_mul_hooked` runs after the 256→4096 sweep and shares the destination MPI `g_mul_x`, which the 4096-bit multiply has left oversized. Use the no-wrap control to price the hook; it costs 0.21–1.30 µs per call. Fixing the ordering is an open follow-up.
- `--wrap` only redirects references the linker resolves, so calls made from *within* the defining translation unit are invisible. `ecp.c` is a separate TU, so the ECP multiply path — the one the stack samples implicated — is counted.

## Files

```
CMakeLists.txt            project; the -Wl,--wrap options, which MUST precede project()
main/compat.h             the one real 3.6.6/4.1.0 difference, absorbed
main/wrap.c               the --wrap hooks and the unwrapped passthroughs
main/bench.c              operands, setup, the benchmark table, the report
sdkconfig.defaults        baseline: accelerator on, fixed-point off
sdkconfig.mpi-off         accelerator off
sdkconfig.fixed-point     fixed-point ECP on
idf{5,6}-mpi-{on,off}[-fp].args   the eight cells
idf{5,6}-mpi-{on,off}-nowrap.args the four uninstrumented controls
run.ps1                   flash, capture, collate

sdk_variant.ps1           put $IDF_PATH's esp_bignum.c into one known state
../../patches/esp-idf-mpi-min-bitlen.patch   the #19027 patch itself
../../patches/esp-idf-mpi-stack-copy.patch   the stack copy asked for in its review
idf6[-s3]-mpi-on-fix[-stackcopy].args   the four stack-copy cells
sdkconfig.cpu{80,160}     CPU frequency, for the crossover-vs-frequency sweep
idf6[-s3]-xover-{80,160,240}.args       the six crossover cells
idf6[-s3]-xover-240-ct-out.args         the same, mbedTLS submodule clean (#873 out)
idf6[-s3]-mulstep-240.args              15..25 limbs, software only: the unroll sawtooth
```

### The one thing the spike plan got wrong

The plan assumed all five primitives were public API in both versions, so one source file would compile unchanged. In mbedTLS 4.1.0 the crypto moved into TF-PSA-Crypto and `bignum.h`, `ecp.h` and `ecdsa.h` are all under `mbedtls/private/`, reachable only with `MBEDTLS_ALLOW_PRIVATE_ACCESS` defined. The symbols are unchanged and still exported, so it is purely an include-path and visibility problem — `main/compat.h` is the whole of the fix, discriminating on `MBEDTLS_VERSION_MAJOR` from `build_info.h` (not IDF's `MBEDTLS_MAJOR_VERSION`, which IDF 6 exports and IDF 5.5 does not).

Hashing is deliberately absent: a fixed 32-byte array stands in for the digest, because ECDSA verify does not care whether it is a real hash and not hashing avoids needing a second compat shim for the one API that genuinely changed (`mbedtls_sha256_*` → `psa_hash_*`).
