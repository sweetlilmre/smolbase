# DRAFT — espressif/esp-idf PR description (BRANCH PUSHED, PR NOT OPENED)

**Branch:** [`sweetlilmre/esp-idf:fix/mpi-hw-min-bit-len`](https://github.com/sweetlilmre/esp-idf/tree/fix/mpi-hw-min-bit-len) — one commit, `components/mbedtls/port/bignum/esp_bignum.c`, +100/−0, one ahead of `espressif:master` and zero behind. **No pull request has been opened.**

**Proposed title:** `mbedtls: don't use the MPI hardware below its crossover size`

---

## Summary

`CONFIG_MBEDTLS_HARDWARE_MPI` is `default y`. On an ESP32 doing ECDSA — which is what a TLS handshake to most of the modern web now does — it makes things **slower**, and it costs much more in a real firmware than an offline benchmark will show you.

Two separate things, measured:

1. **The peripheral is the wrong tool at ECC operand sizes.** A 256-bit multiply costs 19.5 µs through the accelerator against 9.8 µs in software. The crossover is between 256 and 512 bits. `mbedtls_mpi_mul_mpi()` in `components/mbedtls/port/bignum/esp_bignum.c` has an *upper* size bound but no *lower* one, so every 256-bit multiply pays the full ceremony: crypto lock, `periph_module_enable`, two hardware passes, clock disable, unlock.
2. **In a real firmware it is far worse than that ratio suggests**, because the driver path and the elliptic-curve code evict each other from the instruction cache thousands of times per operation. Offline the peripheral costs ~25% on an ECDSA verify. In our shipping firmware it costs **~210%**.

Net effect on our product: turning `CONFIG_MBEDTLS_HARDWARE_MPI` **off** took a TLS handshake to `api.github.com` from 7.36 s to 3.79 s on the same device and image.

## Environment

- ESP32-D0WD-V3, 240 MHz, DIO flash at 40 MHz, no PSRAM
- ESP-IDF v6.0.2 (mbedTLS 4.1.0) and v5.5.5 (mbedTLS 3.6.6) — both versions behave the same way here
- `-Os`, GCC 14.2
- TLS 1.2 ECDHE-ECDSA to `api.github.com`, all-ECDSA certificate chain (P-256 leaf, P-384 root). No RSA operation anywhere in the handshake.

## Evidence

**1. The multiply itself, offline.** Fixed operands, one task pinned to core 1, no WiFi:

| operand bits | software | accelerator |
|---|---|---|
| **256** | **9.78 µs** | **19.49 µs** |
| 512 | 29.22 µs | 22.15 µs |
| 1024 | 100.80 µs | 36.04 µs |
| 2048 | 378.74 µs | 76.51 µs |
| 4096 | 1467.20 µs | 457.92 µs |

The software column scales with the square of operand size. The accelerator column is nearly flat from 256 to 1024, which is the fixed per-call cost. Above the crossover the peripheral earns its place — `mpi_exp_mod` at 2048 bits is 3.6× faster with it. Below it, it does not.

**2. The same call costs twice as much inside a real operation.** A `--wrap` hook with a cycle counter *inside* it, so the figure is the call and not the hook. Same binary, same core, accelerator on:

| | µs per `mbedtls_mpi_mul_mpi` |
|---|---|
| in a tight loop, 4000 calls back to back | **22.9** |
| inside a real ECDSA P-256 verify, 5817 calls | **49.0** |

Nothing about the call changed. Only what runs between the calls.

**3. It is instruction fetch.** Doubling the flash clock, 40 → 80 MHz, nothing else changed:

| core 1, P-256, accelerator on | 40 MHz | 80 MHz | change |
|---|---|---|---|
| tight-loop multiply | 22.9 µs | 24.1 µs | **none** |
| multiply inside a verify | 49.0 µs | 36.5 µs | −25% |
| ECDSA verify total | 703.7 ms | 551.3 ms | −22% |
| TLS handshake | 7358 ms | 6296 ms | −14% |

A resident working set does not care about fetch latency. A thrashing one is proportional to it. Halving the latency removes about half the miss cost, so the total is roughly twice the saving: **~43% of an accelerator-on verify is waiting for flash, against ~24% with the accelerator off.**

**4. What it costs end to end.** Same firmware, same device, same session, A-B-A:

| TLS handshake to api.github.com | |
|---|---|
| `CONFIG_MBEDTLS_HARDWARE_MPI=y` | 7358 ms |
| `CONFIG_MBEDTLS_HARDWARE_MPI` off | **3790 ms** |

Not measurement noise: ECDSA verify on the same two builds is 703.7 ms against 422.6 ms, and the multiply-count is identical (5817 per P-256 verify) in both.

## Mechanism

With the accelerator off, `mbedtls_mpi_mul_mpi()` is plain C in `bignum.c`, next to the code calling it, and the working set is small.

With it on, every call reaches into a second body of code — `esp_bignum.c`, `periph_module_enable`/`disable`, `esp_crypto_mpi_lock`, and the ESP32 DPORT access path with its other-CPU stall. That code and the ECP code trade cache lines **5817 times per P-256 verify**. An offline benchmark never sees this because its binary is small and its working set stays resident; a real firmware with WiFi, lwIP, a filesystem and an HTTP server does not have that luxury.

## The fix, and what it measures

`mbedtls_mpi_mul_mpi()` at `components/mbedtls/port/bignum/esp_bignum.c` already tests an upper bound:

```c
if (hw_words * 32 > SOC_RSA_MAX_BIT_LEN/2) {
    ...fall back...
}
```

The branch adds the matching lower one. Operands below `SOC_MPI_HW_MIN_BIT_LEN` (512) go to a software multiply over `mbedtls_mpi_core_mul()` — the same function the library implementation uses. `MBEDTLS_MPI_MUL_MPI_ALT` compiles that implementation out, and unlike `exp_mod`, which has `MBEDTLS_MPI_EXP_MOD_ALT_FALLBACK` and `mbedtls_mpi_exp_mod_soft()` for exactly this purpose, there is no upstream `mul_mpi` equivalent to call, so the fallback is mirrored in the port. No build-system change is needed: `drivers/builtin/src` is already on the component's include path.

The test is on the real operand size, not on `hw_words`. `mpi_ll_calculate_hardware_words()` rounds up to a multiple of 16 words, so a 256-bit operand is already handed to the peripheral as 512 bits and `hw_words` would never fall below the threshold. That rounding is also why the hardware cost is flat from 256 to 512 bits while the software cost is not.

**Measured, `CONFIG_MBEDTLS_HARDWARE_MPI=y` throughout, same device, all benches `rc=0`:**

| bench | before | after | for reference: hw off |
|---|---|---|---|
| `mpi_mul` 256-bit | 19.69 µs | **11.78 µs** | 11.28 µs |
| `ecdsa_verify` P-256 | 416.99 ms | **334.48 ms** (−19.8%) | 327.75 ms |
| `ecp_mul` P-256 | 195.22 ms | **154.92 ms** (−20.6%) | 151.56 ms |
| `ecdsa_verify` P-384 | 738.47 ms | **704.31 ms** (−4.6%) | — |
| `mpi_mul` 4096-bit | 460.29 µs | 462.31 µs | 1496.12 µs |
| `mpi_exp_mod` 2048-bit | 19.98 ms | **19.45 ms** | 74.50 ms |
| `mpi_exp_mod` 4096-bit | 65.64 ms | 65.39 ms | 262.85 ms |

**ECC runs at software speed and RSA keeps the hardware, in one build.** The ECC columns land within 2% of the accelerator-off build; the RSA rows are unchanged within noise and stay 3–4× faster than software.

End to end, the same firmware doing a TLS 1.2 ECDHE-ECDSA handshake to `api.github.com`:

| | accelerator on | on, with this patch | accelerator off |
|---|---|---|---|
| handshake, core 0 | 7358 ms | **4929 / 5005 ms** | 4599 ms |
| handshake, core 1 | 4063 ms | **2759 ms** | 2735 ms |
| certificate chain verify | 3097 ms | **2087 ms** | — |

P-384 gains least (−4.6%) because 384-bit operands sit close to the crossover — software 21.8 µs against hardware 23.4 µs — so there is little to win there. That is expected and is why the threshold is 512 and not higher.

## Notes on the threshold



`mbedtls_mpi_mul_mpi()` at `components/mbedtls/port/bignum/esp_bignum.c:488` already tests an upper bound:

```c
if (hw_words * 32 > SOC_RSA_MAX_BIT_LEN/2) {
    ...fall back...
}
```

`SOC_MPI_HW_MIN_BIT_LEN` is defined in the port with an `#ifndef` guard so a target can override it. 512 is measured on the ESP32 (LX6); the right constant may differ on the S3 and the RISC-V parts, and naming it `SOC_` is a hint that it probably belongs in `soc_caps.h` once someone measures those. I am happy to move it.

This is a smaller and safer change than revisiting the `default y`, and it costs RSA nothing.

## Prior art

[#8710](https://github.com/espressif/esp-idf/issues/8710) reports the same symptom — a TLS client tripping the task watchdog, fixed by disabling hardware MPI — but with no measurement or mechanism, and it is closed. I could not find a report that quantifies it or identifies the cache component. That was a keyword search, so please treat "not reported" as weak.

## What is not established

- **The cache component is measured on ESP32 (LX6) only.** An S3 was available for other parts of this work but the flash-clock test was not run on it. RISC-V parts are untested.
- **The exact crossover constant** is bracketed between 256 and 512 bits, not pinned.
- **No profiling of which code evicts what.** The conclusion rests on the flash-clock test and on the tight-loop-versus-in-situ split, not on cache performance counters.
- Everything here is `-Os`. `-O2` was tried early in this work and made TLS *slower*, consistent with the same mechanism, but that was not investigated.

## Reproducing

The benchmark harness, the raw captures and the full write-up are available and I am happy to share them. The shortest reproduction is: build any TLS client that talks to an all-ECDSA host, measure the handshake, flip `CONFIG_MBEDTLS_HARDWARE_MPI`, and measure again on the same device in the same session.
