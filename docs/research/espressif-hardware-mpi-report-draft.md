# DRAFT — espressif/esp-idf issue (NOT FILED)

**Proposed title:** `mbedtls: hardware MPI makes ECDSA ~2x slower on ESP32, and costs far more in a real firmware than a benchmark shows (no lower size threshold in esp_mpi_mul_mpi)`

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

## Suggested fix

`mbedtls_mpi_mul_mpi()` at `components/mbedtls/port/bignum/esp_bignum.c:488` already tests an upper bound:

```c
if (hw_words * 32 > SOC_RSA_MAX_BIT_LEN/2) {
    ...fall back...
}
```

There is no matching lower bound. Adding one — falling through to the software implementation below roughly 512 bits — would make the peripheral a win in every case instead of a loss on the one that matters most for TLS today. The crossover measured here is between 256 and 512 bits; the right constant may be chip-dependent and is worth measuring on the S3 and the RISC-V parts too.

That is a smaller and safer change than revisiting the `default y`, and it does not cost RSA anything.

## Prior art

[#8710](https://github.com/espressif/esp-idf/issues/8710) reports the same symptom — a TLS client tripping the task watchdog, fixed by disabling hardware MPI — but with no measurement or mechanism, and it is closed. I could not find a report that quantifies it or identifies the cache component. That was a keyword search, so please treat "not reported" as weak.

## What is not established

- **The cache component is measured on ESP32 (LX6) only.** An S3 was available for other parts of this work but the flash-clock test was not run on it. RISC-V parts are untested.
- **The exact crossover constant** is bracketed between 256 and 512 bits, not pinned.
- **No profiling of which code evicts what.** The conclusion rests on the flash-clock test and on the tight-loop-versus-in-situ split, not on cache performance counters.
- Everything here is `-Os`. `-O2` was tried early in this work and made TLS *slower*, consistent with the same mechanism, but that was not investigated.

## Reproducing

The benchmark harness, the raw captures and the full write-up are available and I am happy to share them. The shortest reproduction is: build any TLS client that talks to an all-ECDSA host, measure the handshake, flip `CONFIG_MBEDTLS_HARDWARE_MPI`, and measure again on the same device in the same session.
