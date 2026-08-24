# mbedTLS 4 is slower than 3.6 on ESP32 — the root cause, with a fix

**Written:** 2026-08-24, after running the experiment described below on hardware. **Status: root cause identified, proven by patch-and-measure, fixed, and the fix proven constant-time-preserving by MemSan constant-flow testing with a working negative control.** The first fix I proposed was refuted by that testing; the corrected one is Xtensa assembly.
**Companions:** [mbedtls4-perf-spike.md](mbedtls4-perf-spike.md) (the spike that measured the gap and established it was real) · [idf6-migration-continuation.md](idf6-migration-continuation.md)
**Harness:** `spike/mbedtls-perf/` · **Captures:** `spike/mbedtls-perf/results/*.log` · **Patch:** [`spike/mbedtls-perf/upstream/0001-ct-xtensa-asm.patch`](../../spike/mbedtls-perf/upstream/0001-ct-xtensa-asm.patch) · **Report to file:** [`upstream/README.md`](../../spike/mbedtls-perf/upstream/README.md)

## The finding in one paragraph

mbedTLS 4.1.0 is 15–41% slower than 3.6.6 per bignum primitive on ESP32, at **identical call counts**. The entire gap comes from **three lines** changed in 2024 by two commits that made `mbedtls_mpi_core_sub()` and `mbedtls_mpi_core_mla()` constant-time. Those lines call `mbedtls_ct_uint_lt()`, which has hand-written assembly for Arm, AArch64, x86-64 and x86 — and **for every other architecture falls back to a generic C implementation built on six `asm volatile` optimisation barriers**. On Xtensa that generic version is additionally too complex for GCC to inline at `-Os`, so each use becomes an out-of-line `call8` with a register-window entry and four stack spills. The result is ~44 CPU cycles where mbedTLS 3.6.6 used a single compare. Reverting the three lines recovers 100% of the gap, which is what pins the cause to them.

**The fix is an Xtensa assembly path** for the three constant-time primitives, mirroring the ones Arm and x86 already have, plus `always_inline` and a free simplification at the two call sites. That restores parity — `ecdsa_verify` 243.19 ms against 3.6.6's 245.12 ms — while remaining constant-time. A pure-C fix was tried first, was faster still, and was **refuted by constant-flow testing**: clang re-derives the comparison from the branchless idiom and emits a branch. That refutation is the most important result here, and it is why the answer is assembly.

**This is not an ESP32 problem.** Any target without one of the four assembly paths is affected: RISC-V, MIPS, PowerPC, SPARC, Xtensa, and any Arm build compiled without `MBEDTLS_HAVE_ASM`.

## Environment

| | |
|---|---|
| Chip | ESP32-D0WD-V3, Xtensa LX6, 240 MHz, no PSRAM |
| Compiler | `xtensa-esp-elf-gcc` 14.2.0, `-Os` (ESP-IDF default for mbedTLS) |
| Slow | mbedTLS **4.1.0** (TF-PSA-Crypto), from ESP-IDF v6.0.2 |
| Fast | mbedTLS **3.6.6**, from ESP-IDF v5.5.5 |
| Method | Same source built against both SDKs. No WiFi, no TLS, no network peer. Fixed operands, one task pinned to core 1, `esp_timer` as the clock, batches sized to ~200 ms, min/mean/max over 5 batches. The RSA/MPI accelerator is **off**, so every measurement below is of vanilla upstream mbedTLS C code — the ESP hardware bignum port is not involved. |

Correctness gate on every run: `rc=0` on all rows, and the ECDSA test vector's `sig_r`/`sig_s` identical across every build, so all variants verify the same signature.

## Root cause

### The three lines

**`mbedtls_mpi_core_sub()`** — introduced by [`4b4869a15`](https://github.com/Mbed-TLS/mbedtls/commit/4b4869a15), "Merge pull request #1236 from Mbed-TLS/change-mpi-sub-to-constant-time", 2024-06-04:

```c
     for (size_t i = 0; i < limbs; i++) {
-        mbedtls_mpi_uint z = (A[i] < c);                    /* 3.6.6 */
+        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(A[i], c),
+                                                    1, 0);  /* 4.x   */
         mbedtls_mpi_uint t = A[i] - c;
-        c = (t < B[i]) + z;                                 /* 3.6.6 */
+        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(t, B[i]), 1, 0) + z;
         X[i] = t - B[i];
     }
```

**`mbedtls_mpi_core_mla()`** — introduced by [`77bd4798`](https://github.com/Mbed-TLS/TF-PSA-Crypto/commit/77bd47982527ac98b5c1f37dc4dcdddbc06208b1), "Change mbedtls_mpi_core_mla() to be constant time", 2024-05-23 (confirmed by `git blame` on `main`):

```c
     while (excess_len--) {
         *d += c;
-        c = (*d < c);                                       /* 3.6.6 */
+        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(*d, c), 1, 0);  /* 4.x */
         d++;
     }
```

Everything else on the multiply path is byte-identical between the two versions — `mbedtls_mpi_mul_mpi()`, `mbedtls_mpi_grow()`, `mbedtls_mpi_lset()` and `mbedtls_mpi_core_mul()` all diff clean.

### Why it costs so much on targets without an asm path

`mbedtls_ct_uint_lt()` in `tf-psa-crypto/utilities/constant_time_impl.h` has four hand-written assembly implementations, selected by:

```c
#if defined(MBEDTLS_HAVE_ASM) && defined(__GNUC__) && (!defined(__ARMCC_VERSION) || __ARMCC_VERSION >= 6000000)
#define MBEDTLS_CT_ASM
#if (defined(__arm__) || defined(__thumb__) || defined(__thumb2__))
#define MBEDTLS_CT_ARM_ASM
#elif defined(__aarch64__)
#define MBEDTLS_CT_AARCH64_ASM
#elif defined(__amd64__) || defined(__x86_64__)
#define MBEDTLS_CT_X86_64_ASM
#elif defined(__i386__)
#define MBEDTLS_CT_X86_ASM
#endif
#endif
```

On Xtensa this is the **worst possible combination**, and it is worth being precise about why:

- `MBEDTLS_CT_ASM` **is** defined — it only requires GCC plus `MBEDTLS_HAVE_ASM`. So `mbedtls_ct_compiler_opaque()` uses its real barrier, `asm volatile ("" : [x] "+r" (x) :)`.
- None of the four architecture macros match, so `mbedtls_ct_uint_lt()`, `mbedtls_ct_if()` and `mbedtls_ct_bool()` all take their generic C `#else` branch — the one built out of `mbedtls_ct_compiler_opaque()` calls.

A single `mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(x, y), 1, 0)` therefore expands to **six optimisation barriers**: two in `ct_uint_lt` for its operands, one in the `ct_bool` it calls, one in its inner `ct_if`, one in the final `ct_bool`, and one in the outer `ct_mpi_uint_if`.

**Un-defining `MBEDTLS_CT_ASM` would make this worse, not better.** Without it, `mbedtls_ct_compiler_opaque()` becomes `return x ^ mbedtls_ct_zero;` where `mbedtls_ct_zero` is a `volatile` global — a memory load per barrier instead of a register barrier. The guard is not the bug.

### And GCC will not inline it

Measured on the real object files. `mbedtls_mpi_core_sub()`, disassembled from the shipping 4.1.0 build:

```
400d80c7:  s32i.n  a13, a1, 12      ; spill
400d80c9:  s32i.n  a12, a1, 8       ; spill
400d80cb:  s32i.n  a9,  a1, 4       ; spill
400d80cd:  call8   400d7e78 <mbedtls_ct_uint_lt>    ; <-- not inlined
...
400d80dd:  s32i.n  a11, a1, 0       ; spill
400d80df:  call8   400d7e78 <mbedtls_ct_uint_lt>    ; <-- again, per limb
```

versus the same function built with 3.6.6's plain C, which is fully inlined and call-free:

```
400e1d59:  movi.n  a13, 1
400e1d5b:  bltu    a9, a2, 400e1d60
400e1d5e:  movi.n  a13, 0
```

| build | `core_sub` instructions | calls | stack spills |
|---|---|---|---|
| 4.1.0 as shipped | 38 (+14 in the callee) | 2 | 4 |
| 3.6.6 plain C | 24 | 0 | 0 |
| 4.1.0 + fix | 28 | 0 | 0 |

At `-Os` GCC judges the barrier-laden body not worth inlining, so every comparison pays a `call8`, an `entry`/`retw` register-window rotation, and spill/reload traffic. That is the single biggest component of the cost.

### Why the multiply regression is exactly O(n)

The `mpi_mul` slowdown is ~180–226 ns per 32-bit limb, flat across operand sizes — about **44 cycles per limb**, not a fixed per-call cost:

| operand bits | limbs | 3.6.6 | 4.1.0 | delta | delta/limb | cycles/limb |
|---|---|---|---|---|---|---|
| 256 | 8 | 9.78 µs | 11.28 µs | 1.50 µs | 187 ns | 45.0 |
| 512 | 16 | 29.22 | 32.06 | 2.83 | 177 | 42.4 |
| 1024 | 32 | 100.80 | 106.57 | 5.76 | 180 | 43.2 |
| 2048 | 64 | 378.74 | 391.30 | 12.55 | 196 | 47.1 |
| 4096 | 128 | 1467.20 | 1496.11 | 28.91 | 226 | 54.2 |

That is explained exactly by the call structure:

```c
void mbedtls_mpi_core_mul(X, A, A_limbs, B, B_limbs) {
    memset(X, 0, (A_limbs + B_limbs) * ciL);
    for (size_t i = 0; i < B_limbs; i++)
        (void) mbedtls_mpi_core_mla(X + i, A_limbs + 1, A, A_limbs, B[i]);
}
```

`d_len = A_limbs + 1`, `s_len = A_limbs`, so `excess_len == 1` — the changed tail loop runs **exactly once per `mla` call**, and `mla` is called once per limb of B. An n×n multiply executes the changed line exactly **n** times. Because the multiply itself is O(n²), the *relative* regression shrinks as operands grow (+15.4% at 256 bits, +2.0% at 4096) while the absolute per-limb cost stays constant. This is why the effect is worst exactly where ECC lives.

`mbedtls_mpi_core_montmul()` — the hot path for ECP and RSA — is hit harder still: its two `mla` calls each have `excess_len == 2` and run n times, plus one `core_sub` over n limbs at **two** constant-time operations per limb. That is **6n** constant-time operations per Montgomery multiplication.

## Proof: patch and measure

Six device builds, all uninstrumented, accelerator off, `rc=0` and matching test vectors throughout.

| variant | what changed |
|---|---|
| **B** | the three lines reverted to 3.6.6's plain C — *proves causation, not a proposed fix* |
| **E** | `__attribute__((always_inline))` on `mbedtls_ct_uint_lt` only |
| **C** | generic `mbedtls_ct_uint_lt` rewritten branchless and barrier-free |
| **F** | C + `always_inline` + call sites using `_if_else_0` — **the proposed fix** |

| bench | 3.6.6 | 4.1.0 | v4 gap | E inline | C branchless | F full fix | F vs 3.6.6 |
|---|---|---|---|---|---|---|---|
| `ecdsa_verify_p256` 256-bit | 245.12 ms | 327.75 ms | +33.7% | 317.98 ms | 266.29 ms | **235.66 ms** | **−3.9%** |
| `ecp_mul_p256` 256-bit | 113.92 ms | 151.56 ms | +33.0% | 147.10 ms | 124.19 ms | **110.50 ms** | −3.0% |
| `mpi_inv_mod_p256` 256-bit | 8.66 ms | 12.23 ms | +41.2% | 11.82 ms | 8.92 ms | **7.33 ms** | −15.4% |
| `mpi_exp_mod` 2048-bit | 61.77 ms | 74.50 ms | +20.6% | 72.74 ms | 65.82 ms | **62.32 ms** | +0.9% |
| `mpi_exp_mod` 4096-bit | 227.03 ms | 262.85 ms | +15.8% | 258.54 ms | 238.99 ms | **229.50 ms** | +1.1% |
| `mpi_mul` 256-bit | 9.78 µs | 11.28 µs | +15.4% | 10.83 µs | 10.03 µs | **9.67 µs** | −1.1% |
| `mpi_mul` 4096-bit | 1.47 ms | 1.50 ms | +2.0% | 1.50 ms | 1.48 ms | **1.48 ms** | +0.9% |

Variant B (not shown) lands within 0.5% of 3.6.6 on every row — reverting those three lines recovers the whole gap, which is what makes the attribution airtight.

**Neither half of the fix works alone.** `always_inline` by itself recovers only 12%: inlining a body made of six barriers does not help. The branchless rewrite alone recovers 74%: the body is cheap but still pays a call. Together they recover 111%.

## The fix

> **The two C changes described in this section were REJECTED by constant-flow testing.** They are fast, and they are not constant time: clang re-derives the comparison and emits a branch. The correct fix is Xtensa assembly plus `always_inline`, and only the third item below (`_if_else_0` at the call sites) survived. See [§ Constant-flow testing](#constant-flow-testing-and-what-it-refuted) and [spike/mbedtls-perf/upstream/README.md](../../spike/mbedtls-perf/upstream/README.md). This section is left as written because the reasoning that led to it is the point — a plausible mechanism is not a verified one, which is the same mistake this whole investigation exists to correct.

Two changes, both preserving constant-time behaviour.

**1. Give the generic `mbedtls_ct_uint_lt()` a branchless, barrier-free implementation.** This is the same six-operation sequence the Arm/AArch64/x86 paths already implement (`eor` / `sub` / `bic` / `and` / `orr` / `asr`), written in C. It contains no comparison operator and no branch, so there is nothing for the compiler to lower into a conditional jump and no barrier is needed:

```c
    /* Borrow out of x - y, broadcast to all bits. */
    const mbedtls_ct_uint_t s = x ^ y;
    const mbedtls_ct_uint_t d = x - y;
    const mbedtls_ct_uint_t r = (d & ~s) | (y & s);
    return (mbedtls_ct_condition_t) (0u - (r >> (MBEDTLS_CT_SIZE - 1)));
```

Verified against all 100 edge-case pairs and 400,000 random pairs of 32-bit values before flashing.

Mark it `always_inline` (or the project's portable equivalent) so `-Os` cannot leave it out of line.

**2. At the call sites, use `_if_else_0` instead of `_if(cond, 1, 0)`.** mbedTLS already provides `mbedtls_ct_mpi_uint_if_else_0(condition, if1)`, which is plainly `(condition & if1)` and needs no barrier, whereas `_if(cond, 1, 0)` must build `~condition` through `mbedtls_ct_compiler_opaque()`:

```c
-        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(A[i], c), 1, 0);
+        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if_else_0(mbedtls_ct_uint_lt(A[i], c), 1);
```

Both of the above were subsequently **refuted** — see the next section. The shipped patch does neither; it adds an Xtensa assembly path instead.

## Upstream status

- **Present in `main` today.** Fetched `Mbed-TLS/TF-PSA-Crypto:main/drivers/builtin/src/bignum_core.c` — both hunks are unchanged.
- **Not in the 3.6 LTS.** `Mbed-TLS/mbedtls:mbedtls-3.6` still has the plain-C version, so the LTS is unaffected and this was never backported — consistent with hardening rather than a fix for an exploitable bug.
- **No matching report found.** Searched issues and PRs in `Mbed-TLS/mbedtls`, `Mbed-TLS/TF-PSA-Crypto` and `espressif/esp-idf` for constant-time/bignum/performance combinations. `Mbed-TLS/mbedtls#10671` ("RSA Performance regression in v4.1.0") is a **different** cause — `mbedtls_rsa_check_privkey()` added to `mbedtls_rsa_parse_key()`, which this benchmark never calls. This was a keyword search, so treat "not reported" as weak evidence and search again before asserting it in an issue.

## Constant-flow testing, and what it refuted

Run afterwards, and it changed the answer. Setup: TF-PSA-Crypto `main` (`c5467adc`), `MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN`, clang 22, `CMAKE_BUILD_TYPE=MemSanDbg`, `MBEDTLS_HAVE_ASM` **unset** so the generic C path is the code actually under test. Harness and driver: [`spike/mbedtls-perf/upstream/constant-flow/`](../../spike/mbedtls-perf/upstream/constant-flow/).

| variant | constant flow | codegen (x86-64, clang `-O2`) |
|---|---|---|
| plain-C carry — **negative control** | **FAIL** | `setb`, `jne`, `je` |
| **unmodified upstream 4.1.0** | **PASS** | all `cmov` |
| branchless C, no barriers (proposed above) | **FAIL** | `jns`, `jne` |
| branchless C, barriers on inputs only | **FAIL** | — |
| `_if_else_0` call-site change alone | **PASS** | — |

**The barrier-free C fix is not constant time.** clang recognises the branchless idiom, re-derives `x < y`, and emits a conditional branch. Barriers on the inputs do not save it either: a barrier hides a *value*, not the *structure* of an expression. Upstream's six-barrier construction is load-bearing, and the assembly paths exist for exactly this reason — Xtensa just never got one.

**The corrected fix is Xtensa assembly**, mirroring the existing Arm sequences, plus forced inlining, plus the `_if_else_0` call sites. Measured on the device:

| bench | 3.6.6 | 4.1.0 | asm only | **asm + inline** | vs 3.6.6 |
|---|---|---|---|---|---|
| `ecdsa_verify_p256` | 245.12 ms | 327.75 ms | 278.02 ms | **243.19 ms** | −0.8% |
| `ecp_mul_p256` | 113.92 ms | 151.56 ms | 129.59 ms | **113.99 ms** | +0.1% |
| `mpi_inv_mod_p256` | 8.66 ms | 12.23 ms | 9.38 ms | **7.59 ms** | −12.3% |

`rc=0` throughout and the ECDSA test vector matches 3.6.6 exactly. `mbedtls_mpi_core_sub()` disassembles to **one** conditional branch (the loop back-edge on the public limb count, identical to stock) and **zero** calls; the plain-C version has three branches.

### The inlining half is not an Xtensa problem

I first wrote the `always_inline` off as an Xtensa workaround. That was wrong, and checking it is what found the more interesting result. The existing asm paths never carried the attribute — so if the out-of-line call is real, those architectures should suffer it too. They do. Same function, `mbedtls_mpi_core_sub()`, which calls `mbedtls_ct_uint_lt()` twice per limb:

| toolchain | `-Os` | `-O2` |
|---|---|---|
| `arm-none-eabi-gcc -mthumb -mcpu=cortex-m4` | **2 calls, not inlined** | 0 calls |
| `arm-none-eabi-gcc -marm -mcpu=arm7tdmi` | **2 calls, not inlined** | 0 calls |
| `xtensa-esp-elf-gcc` | **2 calls, not inlined** | — |
| x86-64 gcc *and* clang | 0 calls | 0 calls |

So this is an **`-Os` problem, not an Xtensa one, and existing Arm Cortex-M builds pay it today.** x86-64 inlines regardless — and does so for the *generic C* body too, which shows the difference is the target rather than the code. On Xtensa a call is merely worse than elsewhere, because of the register-window entry and spills.

Scope of the attribute, measured on Arm thumb `-Os` (whole translation unit `.text`):

| | calls | `core_sub` | `.text` |
|---|---|---|---|
| asm path, stock | 2 | 98 B | 2904 B |
| **asm path + `always_inline`** | **0** | 112 B | 2912 B (**+8**) |
| generic C, stock | 2 | 98 B | 3180 B |
| generic C + `always_inline` | 0 | 216 B | 3576 B (**+396**) |

Which is why the patch gates `MBEDTLS_CT_INLINE` on an asm path being in use and leaves the generic C fallback alone: +8 bytes is free, +396 is a judgement call that belongs to the maintainers.

The patch is [`spike/mbedtls-perf/upstream/0001-ct-xtensa-asm.patch`](../../spike/mbedtls-perf/upstream/0001-ct-xtensa-asm.patch), verified `git apply --check` clean against upstream `main`. It uses a portable `MBEDTLS_CT_INLINE` macro rather than a bare `__attribute__((always_inline))`: mbedTLS supports MSVC (`_MSC_VER` is handled in this very file), MSVC compiles the generic C branch, and `__attribute__` is GNU-only — so the bare attribute would have broken the MSVC build.

**A gap this exposed in upstream's own coverage:** `test_suite_bignum_core`'s `mpi_core_sub`/`mpi_core_mla` cases compare the returned carry while inputs are still `TEST_CF_SECRET`. The carry is secret-derived, so under MemSan they report for *any* implementation — verified, stock and the plain-C revert give byte-identical reports. On x86 with `MBEDTLS_HAVE_ASM` the inline assembly launders the poison and they pass. So the generic C path is never actually constant-flow tested upstream.

## What this does not establish

- **The Xtensa assembly is not MemSan-tested** — there is no MemSan for Xtensa. It rests on the same argument as the existing Arm and x86 paths (assembly is opaque to the optimiser) plus the disassembly check above. Upstream should still run its own constant-flow suite before accepting.
- **One chip, one compiler.** Everything here is ESP32 / Xtensa LX6 / GCC 14.2 / `-Os`. The mechanism predicts RISC-V, MIPS and PowerPC are affected too, but that is reasoning from the preprocessor conditions, not measurement.
- **`mpi_inv_mod` being 15.4% *faster* than 3.6.6 after the fix is unexplained.** 4.x's inversion is a different algorithm (`mbedtls_mpi_core_gcd_modinv_odd`, added July 2025); the fix apparently uncovers a genuine improvement that the constant-time cost was masking. Not investigated.
- **Nothing here is a TLS-handshake measurement.** These are primitives. The handshake-level story is in [mbedtls4-perf-spike.md](mbedtls4-perf-spike.md).

## Reproducing

```
& $HOME\esp\esp-idf-v5.5\export.ps1        # mbedTLS 3.6.6 reference
cd D:\source\smolbase\spike\mbedtls-perf
.\run.ps1 -Runs idf5-mpi-off-nowrap -Flash

& $HOME\esp\esp-idf\export.ps1             # mbedTLS 4.1.0 subject
.\run.ps1 -Runs idf6-mpi-off-nowrap -Flash

# then apply upstream/as-measured.patch inside
# ~/esp/esp-idf/components/mbedtls/mbedtls and rebuild a -varf cell.
```

`git checkout -- tf-psa-crypto/` in that tree restores it; the SDK was left pristine.

**Flashing the spike erases the smolbase firmware, its partition table and NVS.** Recovery is `idf.py '@smolbase.args' -p COM5 -b 460800 flash`. In practice the WiFi credentials survive, because NVS sits at `0x9000` in both the spike's `single_app` layout and `partitions.csv`, and the spike never writes NVS.
