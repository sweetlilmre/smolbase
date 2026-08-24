# Upstream report: mbedTLS 4 constant-time primitives on embedded targets

Everything needed to file this with [Mbed-TLS/TF-PSA-Crypto](https://github.com/Mbed-TLS/TF-PSA-Crypto): a three-patch series, the evidence, and the harness that produced both.

| file | what it is |
|---|---|
| `0001-ct-force-inline-asm-paths.patch` | force inlining on the **existing** asm paths. Independent; fixes an Arm `-Os` cost that exists today. |
| `0002-ct-xtensa-asm-path.patch` | the new Xtensa assembly. Applies on top of 0001 (it adds Xtensa to 0001's macro list). |
| `0003-bignum-core-if-else-0.patch` | `_if_else_0` at two call sites. Independent of both; **the one to drop first**. |
| `BASE-COMMIT.txt` | the mbedTLS commit the series was developed against, as vendored in ESP-IDF v6.0.2. |
| `gen_patches.py`, `gen_series.sh` | regenerate the series in a pristine tree — useful for rebasing. |
| `constant-flow/` | the MemSan constant-flow harness and the inlining probes (below). |
| `../results/*.log` | the device captures every timing here comes from. |

All three verified `git apply` clean against TF-PSA-Crypto `main` (`c5467adc`, 2026-08-13), in order.

## Why three patches

They are three separate decisions and a reviewer should be able to take them separately.

**0001 is arch-neutral and helps existing users.** It changes codegen on Arm, AArch64 and x86, which is a bigger blast radius than the Xtensa work but also the broader win — Arm Cortex-M `-Os` builds pay a call per constant-time comparison today. It needs its own size/speed judgement, and that judgement is not about Xtensa at all.

**0002 is new architecture support.** Purely additive `#elif` branches; it cannot affect any architecture that already has a path.

**0003 is a 1.2% improvement to constant-time bignum code.** That is a poor ratio of review burden to benefit, and it is the piece most likely to be argued about. It is last, it is independent, and dropping it costs the series almost nothing. It should not be able to hold up 0001 and 0002.

Measured contribution of each, ESP32, P-256 ECDSA verify (3.6.6 reference: 245.12 ms; unpatched 4.1.0: 327.75 ms):

| applied | result |
|---|---|
| 0002 alone | 278.02 ms |
| 0001 + 0002 | **243.19 ms** |
| 0003 | ~1.2% in the one configuration where it was isolated |

0001 alone does nothing on Xtensa — there is no asm path for it to act on until 0002. Its standalone value is on Arm, where I have codegen and size figures but no timing.

Full narrative, including how the root cause was found: [docs/research/mbedtls4-ct-bignum-root-cause.md](../../../docs/research/mbedtls4-ct-bignum-root-cause.md).

## The claim, in one paragraph

There are **two independent problems**, both landing on the bignum hot path since [`4b4869a15`](https://github.com/Mbed-TLS/mbedtls/commit/4b4869a15) and [`77bd4798`](https://github.com/Mbed-TLS/TF-PSA-Crypto/commit/77bd47982527ac98b5c1f37dc4dcdddbc06208b1) made `mbedtls_mpi_core_sub()` and `mbedtls_mpi_core_mla()` constant time.

**1. Xtensa has no assembly path.** `mbedtls_ct_bool()`, `mbedtls_ct_if()` and `mbedtls_ct_uint_lt()` have hand-written assembly for Arm, AArch64, x86-64 and x86; everything else uses generic C built from `mbedtls_ct_compiler_opaque()` barriers — six of them for one `mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(a, b), 1, 0)`. Affects RISC-V, MIPS, PowerPC, SPARC and Xtensa.

**2. At `-Os` these functions are not inlined — on Arm as well as Xtensa.** This one affects targets that *do* have an assembly path, and is already costing existing Arm Cortex-M builds. Measured on `mbedtls_mpi_core_sub()`, which calls `mbedtls_ct_uint_lt()` twice per limb:

| toolchain | `-Os` | `-O2` |
|---|---|---|
| `arm-none-eabi-gcc -mthumb -mcpu=cortex-m4` | **2 calls, not inlined** | 0 calls |
| `arm-none-eabi-gcc -marm -mcpu=arm7tdmi` | **2 calls, not inlined** | 0 calls |
| `xtensa-esp-elf-gcc` | **2 calls, not inlined** | — |
| x86-64 gcc *and* clang | 0 calls | 0 calls |

x86-64 inlines regardless, which is presumably why this went unnoticed — and it holds for the generic C body too, so it is a property of the target, not of the code. On Xtensa a call is especially expensive because of the register-window entry and spills.

Net effect on ESP32: **mbedTLS 4.1.0 is 15–41% slower than 3.6.6 per primitive**, at identical call counts.

## Measurements

ESP32-D0WD-V3, Xtensa LX6 @ 240 MHz, GCC 14.2, `-Os`, RSA/MPI accelerator off (so this is pure upstream C, no ESP bignum port involved). Offline: no WiFi, no TLS, fixed operands, one task pinned to core 1, `esp_timer`, batches sized to ~200 ms, min/mean/max over 5 batches. Reference is mbedTLS 3.6.6 from ESP-IDF v5.5.5, same device, same harness.

| bench | 3.6.6 | 4.1.0 | gap | **4.1.0 + patch** | vs 3.6.6 |
|---|---|---|---|---|---|
| `ecdsa_verify` P-256 | 245.12 ms | 327.75 ms | +33.7% | **243.19 ms** | −0.8% |
| `ecp_mul` P-256 | 113.92 ms | 151.56 ms | +33.0% | **113.99 ms** | +0.1% |
| `mpi_inv_mod` P-256 | 8.66 ms | 12.23 ms | +41.2% | **7.59 ms** | −12.3% |
| `mpi_exp_mod` 2048-bit | 61.77 ms | 74.50 ms | +20.6% | **63.45 ms** | +2.7% |
| `mpi_mul` 256-bit | 9.78 µs | 11.28 µs | +15.4% | **9.80 µs** | +0.2% |

Attribution is exact: reverting just the three changed lines to 3.6.6's plain C recovers the whole gap (`ecdsa_verify` 243.96 ms), which is what pins the cause to those lines and nothing else.

The regression is O(n), ~44 cycles per limb, flat from 256 to 4096 bits. `mbedtls_mpi_core_mul()` calls `mla` once per limb of B with `excess_len == 1`, so an n×n multiply runs the changed line exactly n times; `mbedtls_mpi_core_montmul()` does 6n such operations. Because the multiply is O(n²) the *relative* cost shrinks with operand size (+15.4% at 256 bits, +2.0% at 4096) — worst exactly where ECC lives.

## Why the fix is assembly and not C

This is the part worth reading, because the obvious fix is wrong.

The generic `mbedtls_ct_uint_lt()` can be written as the same six-operation branchless sequence the Arm path uses (`eor`/`sub`/`bic`/`and`/`orr`/`asr`), with no barriers at all. It is much faster. **It is also not constant time:** clang 22 at `-O2` recognises the idiom, re-derives `x < y`, and emits a conditional branch. Keeping barriers on the two inputs does not help either — a barrier hides a *value*, not the *structure* of the expression.

Both were caught by MemSan constant-flow testing, with a negative control:

| variant | constant flow |
|---|---|
| plain-C carry (mbedTLS 3.6 style) | **FAIL** — the negative control; proves the harness detects the property |
| **unmodified upstream 4.1.0** | **PASS** |
| branchless C, no barriers | **FAIL** |
| branchless C, barriers on inputs only | **FAIL** |
| `_if_else_0` call-site change alone | **PASS** |

So the existing design is right and the assembly paths are load-bearing. Xtensa simply never got one. Patch 0002 adds it; patch 0001 separately forces inlining where an assembly path exists.

`MBEDTLS_CT_INLINE` (patch 0001) is scoped to the assembly paths on purpose. On the Arm thumb `-Os` measurement it costs **+8 bytes** of `.text`; applying it to the generic C fallback instead costs **+396 bytes**, which is a trade-off for the maintainers to make rather than a clear win.

The Xtensa assembly itself cannot be MemSan-tested — there is no MemSan for Xtensa. It rests on the same argument as the Arm and x86 paths (assembly is opaque to the optimiser), plus a disassembly check: with the series applied, `mbedtls_mpi_core_sub()` contains **one** conditional branch, the loop back-edge on the public limb count, identical to stock; a plain-C implementation has three. It also contains **no calls**, versus two in stock.

## A gap in upstream's own coverage

`test_suite_bignum_core`'s `mpi_core_sub` / `mpi_core_mla` cases cannot be used to test the generic C path as they stand. They compare the returned carry (`test_suite_bignum_core.function:825`) while the inputs are still `TEST_CF_SECRET`, and the carry is secret-derived by construction — so under MemSan they report for *any* implementation, unmodified upstream included. Verified: stock and the plain-C revert produce byte-identical reports there.

On x86 builds with `MBEDTLS_HAVE_ASM` the inline assembly launders the MemSan poison and the cases pass. The consequence is that **the generic C constant-time path is currently not covered by upstream constant-flow testing at all** — it is only ever exercised on architectures that do not use it. That seems worth fixing independently of this patch.

The harness in `constant-flow/` works around it by unpoisoning outputs before touching them, so a report can only come from a branch or memory access *inside* the function.

## Reproducing

Timings, on hardware (see [the harness README](../README.md) — flashing erases the device's firmware):

```
& $HOME\esp\esp-idf-v5.5\export.ps1
cd spike\mbedtls-perf; .\run.ps1 -Runs idf5-mpi-off-nowrap -Flash   # 3.6.6 reference
& $HOME\esp\esp-idf\export.ps1
.\run.ps1 -Runs idf6-mpi-off-nowrap -Flash                          # 4.1.0 subject
```

Constant flow, on a Linux host with clang (no valgrind needed, no packages to install):

```
bash constant-flow/cf_configure.sh          # clone TF-PSA-Crypto main, cmake MemSanDbg
bash constant-flow/cf_prove.sh revert       # negative control -> must FAIL
bash constant-flow/cf_prove.sh stock        # unmodified upstream -> must PASS
bash constant-flow/cf_prove.sh callsites    # the _if_else_0 change -> must PASS
```

Inlining behaviour, no hardware needed:

```
bash constant-flow/inline_probe.sh          # x86-64, asm and generic C, -Os and -O2
bash constant-flow/arm_probe.sh             # Arm thumb/arm, -Os vs -O2
bash constant-flow/arm_probe2.sh            # size cost of always_inline on Arm
```

`cf_configure.sh` unsets `MBEDTLS_HAVE_ASM` deliberately: with it set, `mbedtls_ct_uint_lt()` takes the x86-64 assembly path and the generic C — the code actually under test — is never compiled. It also unsets `MBEDTLS_AESNI_C`/`AESCE`/`PADLOCK`, which `#error` without assembly.

## Before filing

- **Search first.** No matching report was found in `Mbed-TLS/mbedtls`, `Mbed-TLS/TF-PSA-Crypto` or `espressif/esp-idf`, but that was a keyword search. `Mbed-TLS/mbedtls#10671` ("RSA Performance regression in v4.1.0") is a *different* cause — `mbedtls_rsa_check_privkey()` in key parsing — and should not be conflated with this.
- **Scope honestly.** Measured on one chip, one compiler, one optimisation level. RISC-V/MIPS/PowerPC are implicated by the preprocessor conditions, not by measurement.
- **The 3.6 LTS is unaffected** — it still has the plain-C version and never had this backported.
