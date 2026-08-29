# mbedTLS 4 TLS performance — the complete record

**Written:** 2026-08-24/25, rewritten 2026-08-25 once the investigation finished. **Read this first if you are picking this up with no context.** It is the single document for the TLS performance work: what was asked, what caused it, how three wrong answers happened along the way, and what is left.
**Branch:** `idf6-migration`. Working tree clean except the probes under [*Dirty state*](#dirty-state--revert-before-the-branch-ships), which are deliberate. **The IDF 6 SDK tree is instrumented too** — check that section before trusting any measurement.
**Companions:** [mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md) is the evidence log for the constant-time strand and the upstream patch. [idf6-migration-continuation.md](idf6-migration-continuation.md) is the wider migration state.

> An earlier version of this file was written in layers, as parts 1a through 1f, each correcting the one before. Three of those conclusions were wrong and were retracted. That history is in git; the corrections that matter are folded into [Part 4](#part-4--how-three-wrong-answers-happened), because how they happened is the most transferable thing here.

---

## The answer

The native ESP-IDF build appeared to be roughly twice as slow as the arduino-esp32 build it replaced: 8.49 s against 4.12 s for `/api/update/check`. **It had three independent causes, none of them mbedTLS 4 being broadly slower, and all three are now measured.**

| cause | size | status |
|---|---|---|
| our own unbuffered `ClientReader` — one byte per TLS read | ~2.07 s | fixed |
| the RSA/MPI accelerator being wrong for ECC-sized operands | the dominant term | fixed by config |
| the mbedTLS 4 constant-time bignum regression | uniform ~30% on EC operations | patch open upstream |

Two further findings came out of chasing the remainder, and both matter more than the original question:

- **Roughly half of an ECDSA verify on this chip is waiting for instruction fetch, not computing.** With the accelerator on it is ~43%; with it off, ~24%. The accelerator is far worse in a real firmware than in a benchmark because each call drags a second body of code through a small instruction cache, 5817 times per P-256 verify.
- **Elliptic-curve maths on core 0 costs 1.75× what it costs on core 1**, because the WiFi and lwIP tasks deschedule it. The same handshake is 3.8 s on core 0 and 2.1 s on core 1. This is the one item with a shipping consequence still open.

**Smolbase is now faster than the firmware it replaced**, at every level — primitive, certificate chain, and whole handshake. The old build's remaining advantage in one configuration turned out to be a code-placement accident worth ±20%, which is [demonstrated by moving it](#27--the-arduino-builds-advantage-is-code-placement).

## Where the numbers stand

Shipped configuration (`HARDWARE_MPI` off, `ECP_FIXED_POINT_OPTIM` on), measured on the device:

| | arduino v0.3.3 (3.6.6) | Smolbase (4.1.0) |
|---|---|---|
| P-256 ECDSA verify, core 1 | 601 ms *(its own config)* | **237 ms** |
| P-384 ECDSA verify, core 1 | 570 ms *(its own config)* | **505 ms** |
| certificate chain verify | ~1600 ms | **~1440 ms** |
| TLS handshake, core 0 | ~3660 ms | ~3790 ms |
| TLS handshake, core 1 | not measured | **~2140 ms** |

The arduino build cannot be run with the accelerator off — its mbedTLS is precompiled with `MBEDTLS_MPI_MUL_MPI_ALT` baked in — so its column is its own shipped configuration and no other is reachable. Like-for-like library comparison is in [§2.5](#25--the-version-regression-is-a-uniform-30-on-both-curves).

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration` |
| ESP-IDF 6.0.2 | `~/esp/esp-idf` → mbedTLS 4.1.0. `& $HOME\esp\esp-idf\export.ps1` |
| ESP-IDF 5.5.5 | `~/esp/esp-idf-v5.5` → mbedTLS 3.6.6. `& $HOME\esp\esp-idf-v5.5\export.ps1` |
| **arduino baseline** | worktree `D:\source\smolbase-v033`, detached at `v0.3.3` (`22f1580`). Builds with `pio` (pioarduino). **Its probes are uncommitted — do not `git checkout` it.** |
| Device | ESP32-D0WD-V3 on **COM5**, RTS auto-reset. Running Smolbase `0.4.0-dev`, shipped config. IP `10.0.0.32`. |
| Second device | ESP32-S3 on **COM9** — a Waveshare "xiaozhi" board, **not ours**, restored to its original firmware. Leave alone. |
| Upstream clone | `D:\source\TF-PSA-Crypto`, branch `constant-time-embedded-perf`. `origin` = the fork, `upstream` = Mbed-TLS. |
| MemSan clone | `~/ctflow/tfpsa` in WSL Ubuntu, for constant-flow testing. |
| uncrustify 0.75.1 | `~/uncrustify-build/uncrustify/build/uncrustify`. The version `code_style.py` pins; Ubuntu's 0.78.1 is refused. |

Only one SDK can be active per shell, and `export.ps1` is not idempotent across versions — use a fresh shell to switch.

---

# Part 1 — The original problem

`/api/update/check` makes one TLS 1.2 ECDHE-ECDSA connection to `api.github.com` and reads ~30 KB of release JSON. On the arduino build it took 4.12 s. On the first native build it took 8.49 s, and the task watchdog fired.

**The negotiated suite is `TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256` over an all-ECDSA chain** (`*.github.com` EC-256, Sectigo EC-256/384). There is no RSA operation in a GitHub handshake at all. **The ESP32 has no ECC accelerator** — `MBEDTLS_HARDWARE_ECC` and `MBEDTLS_HARDWARE_ECDSA_VERIFY` both depend on SOC capabilities this chip lacks, so elliptic-curve maths is software on every IDF version.

The three causes, in the order they were found:

**1. `ClientReader` was unbuffered.** One byte per `esp_http_client_read`, through mbedTLS, while ArduinoJson scans byte by byte. GitHub's ~30 KB response became ~30,000 TLS round trips, measured at **2.07 s**. The arduino path never paid this because it passed an arduino-esp32 `NetworkClient` Stream, which owns a 1436-byte `NetworkClientRxBuffer` for exactly this purpose. Introduced by migration phase 4a; fixed at the same size in the same place. **Ours, not mbedTLS's.**

**2. The accelerator was the wrong tool.** `CONFIG_MBEDTLS_HARDWARE_MPI` drives the ESP32's **RSA** peripheral. Its only effect on an ECDSA handshake is intercepting `mbedtls_mpi_mul_mpi`, and the ESP port applies no lower size threshold: every 256-bit multiply pays a crypto-lock acquire, a clock enable, two hardware passes, a clock disable and a lock release. At 256 bits the peripheral costs 19.5 µs against software's 9.8 µs. The crossover is between 256 and 512 bits, and P-256 sits on the losing side. Details in [Part 3](#part-3--the-accelerator-and-the-constant-time-regression).

**3. mbedTLS 4.1.0 is genuinely slower than 3.6.6** — a uniform ~30% on EC operations, root-caused to three lines changed upstream in 2024 and now a draft pull request. See [§2.5](#25--the-version-regression-is-a-uniform-30-on-both-curves) and [Part 3](#the-constant-time-root-cause-and-the-upstream-patch).

Turning the accelerator off and fixed-point ECP on is worth **1.7–1.8× on both libraries**, so it more than pays for cause 3. That lever was only available because the migration went native: under the arduino framework, mbedTLS ships precompiled and its configuration cannot be changed without rebuilding the whole framework.

---

# Part 2 — The evidence

Every figure below is from the device unless it says harness. Repeated measurements are two consecutive `/api/update/check` requests in one session; A-B-A where a lever is being crossed.

## 2.1 — `x509_crt_verify` is its two signature verifies, and nothing else

Certificate chain verification is the largest single component of the handshake, so it was taken apart from the inside: cycle counters (`rsr ccount`, needing no component dependency) around `psa_hash_compute`, `mbedtls_pk_can_do_psa`, `mbedtls_pk_verify_ext` and `x509_crt_check_parent` in `library/x509_crt.c`, and around `psa_import_key` / `psa_verify_hash` / `mbedtls_ecdsa_der_to_raw` / `psa_destroy_key` in `tf-psa-crypto/extras/pk_wrap.c`. The counters are zeroed immediately before the benchmark's single `mbedtls_x509_crt_verify`; free-running from boot they read every handshake since.

| inside one `mbedtls_x509_crt_verify` (1436 ms) | | calls |
|---|---|---|
| `mbedtls_pk_verify_ext` | **1434.9 ms** | 2 |
|  → `psa_verify_hash` | **1432.3 ms** | 2 |
|  → `psa_import_key` | 2.1 ms | 2 |
|  → `psa_destroy_key` | 0.21 ms | 2 |
|  → `mbedtls_ecdsa_der_to_raw` | 0.13 ms | 2 |
| `psa_hash_compute` over the TBS | 1.0 ms | 2 |
| `mbedtls_pk_can_do_psa` | 0.15 ms | 2 |
| `x509_crt_check_parent` | 0.10 ms | 3 |
|  → `x509_name_cmp` | 0.05 ms | 3 |

**99.8% is the two signature verifies.** Parsing, name comparison, the CA-bit and key-usage checks, hashing, and the per-certificate PSA key import together are 4 ms of 1440. There is no chain-walk overhead, and there never was — see [Part 4, wrong answer 1](#wrong-answer-1--subtracting-a-benchmark-figure-from-a-firmware-figure).

## 2.2 — PSA costs the same as the legacy API

mbedTLS 4 routes every certificate signature check through PSA. The arduino build's 3.6.6 does not define `MBEDTLS_USE_PSA_CRYPTO` at all and uses the legacy `mbedtls_ecdsa_verify`. That asymmetry looked promising. It is not one. `src/core/spike_psa_vs_legacy.cpp` verifies the same signature both ways in the same binary, on the RFC 6979 keys the harness uses:

| | legacy `mbedtls_ecdsa_verify` | PSA import + verify + destroy | ratio |
|---|---|---|---|
| P-256, core 1 | 237.4 ms | 240.3 ms | 1.01 |
| P-384, core 1 | 504.5 ms | 508.6 ms | 1.01 |

The arduino build says the same (0.95–1.02 across every context). PSA is not a cost on either version.

## 2.3 — Elliptic-curve maths costs 1.75× on core 0

Same binary, same operands, differing only in which task runs the call:

| P-256 ECDSA verify | legacy | PSA |
|---|---|---|
| pinned core 1, priority 5 | **237 ms** | 240 ms |
| pinned core 0, priority 5 | **418 ms** | 419–492 ms |
| on the httpd task (unpinned; behaves as core 0) | 406–506 ms | 411–496 ms |
| **pinned core 0, priority 24** | **236.7 ms** | 240.2 ms |
| `spike/mbedtls-perf/` harness, same levers | 234 ms | — |

Priority 24 is above the WiFi task (23) and the lwIP task (18). At that priority core 0 is exactly as fast as core 1, so **the crypto is not slower on core 0; it is descheduled.** About 44% of the wall clock of a verify on core 0 is spent running the network stack instead — with WiFi merely associated and idle, since this benchmark touches no socket.

It carries through to a whole handshake. Three full `esp_tls_conn_new_sync` connections to `api.github.com`, A-B-A in one session, on two consecutive requests:

| | run 1 | run 2 |
|---|---|---|
| core 0 (A) | 3869 ms | 3771 ms |
| **core 1 (B)** | **2147 ms** | **2132 ms** |
| core 0 (A again) | 3710 ms | 3800 ms |

Every build pays this, arduino included (1.37× there), and priority removes it in every build.

**What to do about it is an open decision.** `httpServer.config.core_id = 0` is deliberate: ADR 0001 puts network work on core 0 precisely so consumer code keeps core 1. Moving a 2.1 s handshake onto core 1 moves a 2.1 s stall onto the task that drives the Screen. Three options, none yet measured against the App loop's frame budget:

- **Run only the update/TLS work on core 1**, on its own task, leaving the rest of the web server on core 0.
- **Raise the priority of the task doing the handshake** while it handshakes. Equally effective, and considerably more dangerous: priority 24 starves the WiFi task, and the benchmark held it for 240 ms at a time, not 2 s.
- **Accept it.** 3.8 s with no watchdog trips is a working system, and this is one HTTP request a day.

## 2.4 — Identical multiply counts, identical multiply prices

Counted with the linker's `--wrap`, which works against the arduino build's precompiled mbedTLS because it acts at link time. Counting only — no timers in the hook — so the harness's instrumentation-overhead trap does not apply. `ecp.c` is a separate translation unit from `bignum.c` and `esp_bignum.c`, so the ECP multiply path is counted.

Core 1, accelerator on, fixed-point off, same probe source, same device:

| | arduino 3.6.6 | ours 4.1.0 |
|---|---|---|
| `mul_mpi` calls per P-256 verify | **5817** | **5817** |
| `mul_mpi` calls per P-384 verify | **8569** | **8569** |
| µs per 256-bit `mul_mpi` | 21.7 | 24.4 |
| µs per 384-bit `mul_mpi` | 23.0 | 23.1 |
| fast reduction (`grp.modp`) present | yes, both curves | yes, both curves |

Identical to the digit, and 5817 matches the harness exactly. So the version difference is not the ECP layer, not the accelerator path, and not a missing fast reduction. It is in the non-multiply arithmetic — the modular reduction and the add/subtract paths, which is where the constant-time regression lives and where the upstream patch points.

Heap traffic was also eliminated here, using `mbedtls_platform_set_calloc_free` (public API in both versions, `MBEDTLS_PLATFORM_MEMORY` on in both builds, so no library patch and it works on the arduino side too): **1455 allocations per P-256 verify, 1–8% of the time.**

## 2.5 — The version regression is a uniform 30% on both curves

The original benchmark matrix had P-384 in the accelerator-**on** cells only, which is the one configuration where the harness and the firmware disagree. Two cells were added — `idf5-mpi-off-fp-nowrap` and `idf6-mpi-off-fp-nowrap`: the shipped levers, no `--wrap` hook, both SDKs, both built from source:

| | mbedTLS 3.6.6 | mbedTLS 4.1.0 | 4.1.0 ÷ 3.6.6 |
|---|---|---|---|
| `ecdsa_verify` P-256 | 172.7 ms | 230.5 ms | **1.34** |
| `ecdsa_verify` P-384 | 365.3 ms | 475.0 ms | **1.30** |
| `ecp_mul` P-256 | 41.3 ms | 53.9 ms | 1.31 |
| `ecp_mul` P-384 | 87.5 ms | 101.2 ms | 1.16 |

**Uniform, about 30%, both curves.** That is the shape a per-limb constant-time penalty should have, and it is the first evidence that the upstream series is correctly *sized* as well as correctly aimed.

The ruler check holds: the firmware reads 237 ms and 505 ms for the same two calls, 3% and 6% off the harness. In this configuration the harness transfers, and these numbers describe what ships.

[§2.7](#27--the-arduino-builds-advantage-is-code-placement) reaches the same uniform figure from a completely different direction.

## 2.6 — About half of a verify is instruction fetch

The `--wrap` hook was given a cycle counter, accumulated **inside** the hook so the figure is the real call and not the hook. The tight-loop multiply benchmark and the real ECDSA verify then both go through that same hook, in the same binary, on the same core, so their per-call figures are directly comparable — a number that cannot be explained away by binary layout, hook cost, or which build it came from.

Core 1, priority 5, P-256, accelerator on, one binary:

| | µs per `mbedtls_mpi_mul_mpi` call |
|---|---|
| in a tight loop, 4000 calls back to back | **22.9** |
| inside a real ECDSA verify, 5817 calls | **49.0** |

**The identical call costs 2.14× more inside a verify.** Nothing about the call changed; only what ran between the calls. The accelerator-off control says the same — 11.0 µs in a loop, 24.0 µs in a verify, 2.18× — so interleaving itself is general.

Doubling the flash clock names the mechanism. 40 MHz to 80 MHz, everything else identical:

| core 1, P-256, accelerator on | 40 MHz | 80 MHz | change |
|---|---|---|---|
| tight-loop multiply | 22.9 µs | 24.1 µs | **none** |
| multiply inside a verify | 49.0 µs | 36.5 µs | −25% |
| verify total | 703.7 ms | 551.3 ms | −22% |
| non-multiply part of the verify | 418.6 ms | 338.8 ms | −19% |
| handshake, core 0 | 7358 ms | 6296 ms | −14% |

**A tight loop does not care how fast the flash is. A real verify cares a great deal.** That is the signature of instruction-cache misses: a resident working set is unaffected by fetch latency, a thrashing one is proportional to it. Halving the latency removes about half the miss cost, so the total is roughly twice the saving:

| | miss cost | share of the operation |
|---|---|---|
| P-256 verify, accelerator **on** | ~305 ms of 704 ms | **43%** |
| P-256 verify, accelerator **off** | ~101 ms of 423 ms | **24%** |
| handshake on core 0, accelerator **on** | ~2124 ms of 7358 ms | **29%** |

Why the accelerator makes it so much worse: with it off, `mbedtls_mpi_mul_mpi` is plain C in `bignum.c` beside the code that calls it. With it on, each call reaches into a second body of code — `esp_bignum.c`, the peripheral clock control, the crypto mutex, and the ESP32 DPORT access path with its other-CPU stall — and that code and the elliptic-curve code trade cache lines 5817 times per verify. The harness never sees it because its binary is roughly a fifth the size and it does nothing else, so it measures the honest cost of the peripheral and none of the cache cost.

**Code volume is not the difference between the two builds.** Summed `.text` of the ECP and bignum symbols: 22.1 KB in ours, 22.5 KB in the arduino build. Equal.

**Ruled out by measurement:** preemption (priority 24 changes nothing — 681 ms against 704 ms), heap traffic (1–8%), the ECP layer, the multiply's own price, and a missing fast reduction.

**Shipping an 80 MHz flash clock does not pay.** Measured in the shipped configuration: core 1 gives 2137 ms against 2132–2147 ms at 40 MHz — identical; core 0 is perhaps 5% better, and those two builds differ by the instrumentation hooks, so even that is not solid. This is the mechanism confirming itself: with the accelerator off there are few misses to accelerate. **Not recommended.**

## 2.7 — The arduino build's advantage is code placement

With the accelerator on, the arduino build does P-384 in 570 ms where we take 1047 ms, while the two builds are level on P-256. Since both pay the instruction-fetch cost, that needed explaining.

**The flash-clock test finds it.** Core 1, accelerator on, 40 MHz against 80 MHz:

| | 40 MHz | 80 MHz | change |
|---|---|---|---|
| **arduino, P-256** | 601 ms | 441 ms | **−27%** |
| **arduino, P-384** | 570 ms | **572 ms** | **none** |
| ours, P-256 | 704 ms | 551 ms | −22% |
| ours, P-384 | 1141 ms | 947 ms | −17% |

The arduino build's P-384 is the only cell in the matrix that does not care how fast the flash is. Its code is resident in the instruction cache. Everything else — including the arduino build's own P-256 — is fetching from flash.

**And moving the code breaks it.** A ~1.4 KB block of real, reachable code added to the arduino build's application objects shifts every library address after them. `ecp_mod_p384` moved from `0x4016ae90` to `0x4016b420`; `ecp_mod_p256` moved by the same 1424 bytes. Nothing else changed:

| arduino, core 1, accelerator on | before the shift | after the shift |
|---|---|---|
| P-256 verify | 601 ms | **741 ms** (+23%) |
| P-384 verify | 570 ms | **684 ms** (+20%) |
| tight-loop multiply, 256-bit | 21.7 µs | 20.9 µs (unchanged) |
| tight-loop multiply, 384-bit | 23.0 µs | 23.8 µs (unchanged) |

**Moving mbedTLS 1424 bytes cost the arduino build 20–23% on both curves, and did not touch the multiply.** That is causal, not correlational: the effect follows the placement. After the shift our build is *faster* than the arduino build on P-256 (704 ms against 741 ms).

**And with the cache cost removed the two libraries agree.** Subtracting the miss cost — about twice the flash-clock saving:

| | miss cost | compute only | ours ÷ arduino |
|---|---|---|---|
| arduino, P-256 | ~320 ms (53%) | 281 ms | |
| ours, P-256 | ~305 ms (43%) | 399 ms | **1.42** |
| arduino, P-384 | **~0 ms (0%)** | 570 ms | |
| ours, P-384 | ~388 ms (34%) | 753 ms | **1.32** |

A uniform 1.32–1.42×, matching [§2.5](#25--the-version-regression-is-a-uniform-30-on-both-curves) and matching what the harness reports for this configuration (1.40 and 1.42). Compute-only also restores the sane curve ratio the raw figures destroyed: P-384 costs 2.03× P-256 on the arduino build and 1.89× on ours. A bigger curve costs more, in both, once the cache noise is out.

**So the old firmware was not better engineered here. It got lucky with where the linker put one function, in a configuration that is the wrong configuration for both builds, and the luck is worth ±20% and evaporates if anything shifts.**

## 2.8 — The 875 ms does not survive the padding experiment

**Run 2026-08-29**, closing both open items on [PR 873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873): does the ~875 ms accelerator-on handshake regression belong to the series (code growth) or to placement, and what does the series cost in Xtensa `.text`?

Six firmware builds in the 875 ms configuration (`HARDWARE_MPI=y`, `ECP_FIXED_POINT_OPTIM` off): the series unapplied (U) and applied (P), each at three placements — no pad, ~1.5 KB, ~3.2 KB of reachable-but-never-executed code linked in the app objects (`src/core/spike_pad.cpp`), which shifts every mbedTLS symbol without changing a byte of it (verified by nm: same sizes, same call8 counts, same summed `.text` within each family). Every image fingerprinted before flashing — `mbedtls_mpi_core_sub` 88 B / 2 call8 stock, 95 B / 0 call8 with the series — using a byte-walking decoder (`count_calls.py`), because **both Windows xtensa objdump and gdb currently decode every ELF as raw hex words** (the Linux `.so` dynconfigs cannot load; the documented trap, now with a cause). OTA-flashed, 4 reps of `/api/update/check` per cell, all bodies validated, U0 re-flashed and re-run last (U0R) as the session-drift control: it agrees with U0 to **0.01%** on the offline verifies.

Means of 4, `PAD-*.serial.log` in `spike/mbedtls-perf/results/`:

| cell | `ecp_mod_p256` at | TLS phase (httpd) | handshake core 1 | chain verify | P-256 verify core 1 | P-384 verify core 1 |
|---|---|---|---|---|---|---|
| U0 | 0x40132c5c | 8325 ms | 4702 ms | 3445 ms | 702 ms | 1145 ms |
| U1 (+1528 B) | 0x40133254 | 8488 ms | 4650 ms | 3458 ms | 713 ms | 1171 ms |
| U2 (+3180 B) | 0x401338c8 | 8219 ms | 4512 ms | 3376 ms | 747 ms | 1040 ms |
| P0 | 0x40132920 | 6800 ms | 3685 ms | 2986 ms | 1043 ms | 589 ms |
| P1 | 0x40132f18 | 6128 ms | 3460 ms | 2610 ms | 858 ms | 584 ms |
| P2 | 0x40133588 | 7140 ms | 3772 ms | 3091 ms | 995 ms | 673 ms |
| U0R (= U0 image) | 0x40132c5c | 8249 ms | 4699 ms | 3430 ms | 702 ms | 1147 ms |

Three findings:

1. **The 875 ms does not reproduce — the sign flips.** Every patched cell beats every unpatched cell end to end, by 1.1–2.4 s on the TLS phase and ~1 s on the core-1 handshake. The PR body's warning ("do not expect the same magnitude or necessarily the same sign") is now a measurement, not a caveat. The original 875 ms was a true A-B-A of *those two binaries*; it is not a property of the series.
2. **Placement alone moves the handshake by more than 875 ms.** Within the patched family — byte-identical library, pad-only differences — the TLS phase spans 6128–7140 ms (~1.0 s). At the primitive level the same binary shows the patch making P-256 *slower* (+150–340 ms) and P-384 *faster* (−370–590 ms) simultaneously — impossible as arithmetic (the harness has the series at parity on both curves in exactly this configuration, §*Second update*), and exactly what per-function cache alignment produces. The tight-loop multiply is flat across all seven cells (22–25 µs), as it should be.
3. **The series shrinks Xtensa code.** Summed `.text` of the ecp/bignum/constant-time symbols: 21190 → 21048 B (**−142 B**); whole image 1465904 → 1465760 B (**−144 B**). The out-of-line `mbedtls_ct_uint_lt` copies (3 × 41 B) disappear, as the upstream README's Arm analysis predicted. There is no code growth for the code-growth hypothesis to stand on.

**The 875 ms question is closed: it was placement.** What the accelerator-on configuration actually shows, once the series is in, is the same story as everywhere else in this record — on this chip, timing is a property of the binary, and the padding experiment now brackets how much: ~±0.5 s on a handshake, ±20% on a single primitive, from moving code nobody executes.

---

# Part 3 — The accelerator and the constant-time regression

This is the strand that ran first, as a pre-registered spike. Its conclusions stand.

## Primitive benchmarks

Offline, no WiFi, no TLS, no network peer. Fixed operands, one task pinned to core 1, `esp_timer`, batches sized to ~200 ms, min/mean/max over 5 batches. Harness: `spike/mbedtls-perf/`.

**C2 — does mbedTLS 4's ECP call `mbedtls_mpi_mul_mpi` more often than 3.6.6? REFUTED.** Both versions make exactly 5817 calls per P-256 verify (3912 with fixed-point ECP), and exactly 2905 per `ecp_mul` (1000 with it). Identical to the digit, in every lever combination, on both SDKs, and later confirmed in the firmware on both curves ([§2.4](#24--identical-multiply-counts-identical-multiply-prices)). It also shows the mechanism of the fixed-point win: it cuts call *count*, where turning the accelerator off cuts cost *per call*.

**C3 — at ECC operand sizes, does the accelerator cost more than a software multiply? CONFIRMED.** `mpi_mul` mean µs per call, unhooked:

| operand bits | 3.6.6 software | 3.6.6 accelerator | 4.1.0 software | 4.1.0 accelerator |
|---|---|---|---|---|
| **256** | **9.78** | **19.49** | **11.28** | **19.69** |
| 512 | 29.22 | 22.15 | 32.05 | 22.29 |
| 1024 | 100.80 | 36.04 | 106.56 | 36.18 |
| 2048 | 378.74 | 76.51 | 391.29 | 76.65 |
| 4096 | 1467.20 | 457.92 | 1496.11 | 462.30 |

The software column scales with the square of operand size; the accelerator column is nearly flat from 256 to 1024 — that flatness *is* the fixed per-call ceremony, exactly as the ESP port's lack of a lower size threshold predicts.

Above the crossover it earns its place. `mpi_exp_mod` with `e = 65537`:

| | 3.6.6 sw | 3.6.6 hw | 4.1.0 sw | 4.1.0 hw |
|---|---|---|---|---|
| 2048-bit | 61.77 ms | 17.03 ms (**3.6×**) | 74.50 ms | 19.98 ms (**3.7×**) |
| 4096-bit | 227.03 ms | 54.56 ms (**4.2×**) | 262.85 ms | 65.67 ms (**4.0×**) |

So the shipped config gives up **+197 ms per RSA-4096 verify**. Affordable: a chain does one or two such verifies while an ECDHE handshake does thousands of 256-bit multiplies. GitHub's chain does none.

**The accelerator was always wrong for this workload, in both versions.** The cell the precompiled arduino libraries made unreachable, measured at last (uninstrumented, fixed-point off):

| | accelerator on | accelerator off | gain |
|---|---|---|---|
| **mbedTLS 3.6.6** | 298.22 ms | **245.12 ms** | **−17.8%** |
| **mbedTLS 4.1.0** | 366.86 ms | 327.75 ms | −10.7% |

3.6.6 gains *more* from turning it off than 4.1.0 does. And [§2.6](#26--about-half-of-a-verify-is-instruction-fetch) shows the harness understates the cost badly: in the shipping firmware the peripheral costs 210%, not 25%.

**#119 is still safe, by a simpler route.** That failure was the RSA accelerator being unable to complete the 4096-bit verify in GitHub's ISRG Root X1 cross-signed CDN chain. With no hardware MPI there is no hardware limit to exceed — every RSA op is software, so it just works. `LARGE_KEY_SOFTWARE_MPI` existed only to add that fallback, depends on `HARDWARE_MPI`, and is gone with it deliberately. Validated by a full self-update.

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

## The constant-time root cause, and the upstream patch

Three lines, changed in 2024, are the whole primitive-level version gap: `mbedtls_mpi_core_sub()`'s loop and `mbedtls_mpi_core_mla()`'s carry tail now call `mbedtls_ct_uint_lt()` + `mbedtls_ct_mpi_uint_if()` where 3.6.6 used a plain comparison. Reverting those three lines recovers ~100% of the primitive gap in **both** accelerator configurations. `mbedtls_ct_uint_lt()` has hand-written assembly for Arm, AArch64, x86-64 and x86 and generic barrier-based C for everything else; at `-Os` it is also left out of line, on Arm as well as Xtensa. Full detail, disassembly and constant-flow results: [mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md).

A C-level fix was tried and **refuted by constant-flow testing** — clang re-derives the comparison from a branchless idiom and emits a branch. The shipped answer is an Xtensa assembly path plus forced inlining.

End to end on the shipped config, A-B-A: unpatched 4198 ms, patched **3391 ms**, unpatched again 4253 ms. **830 ms, 19.8%.**

## A harness defect the spike found in itself

`mpi_mul_hooked` appeared to show the instrumentation costing 11.08 µs per call with the accelerator on and 2.33 µs with it off — an artefact the same order as the effect under test, pointing the same way, and capable of manufacturing the whole result. Four uninstrumented control cells (`-D SPIKE_NO_WRAP=1`) settled it: the **real** hook cost is 0.21–1.30 µs per call, and a build with no hook linked at all reproduces the same margin. The margin is benchmark ordering — `mpi_mul_hooked` runs after the 4096-bit sweep and shares an oversized destination MPI. **`mpi_mul_hooked` does not price the instrumentation; the no-wrap control does.** Fixing the ordering is an open follow-up that affects nothing above.

---

# Part 4 — How three wrong answers happened

This is the part worth reading. Each of these survived review, was written up as a finding, and was wrong. All three are the same mistake in different clothes: **a number was compared against another number that was not measured the same way, and nothing in the numbers said so.**

## Wrong answer 1 — subtracting a benchmark figure from a firmware figure

**The claim.** The arduino-vs-ours handshake gap was +2650 ms, of which 1487 ms was attributed and **1163 ms was an unexplained residual**. Separately, `mbedtls_x509_crt_verify` took 1270 ms on 3.6.6 and 2527 ms on 4.1.0, of which the two signature verifies accounted for only ~1156 ms, leaving **~1370 ms of chain-walk overhead** to find.

**Why it was wrong.** Both figures were produced by subtracting `spike/mbedtls-perf/` primitive numbers from firmware measurements. The harness pins its task to core 1 with WiFi absent; the firmware runs the same call on core 0 with the WiFi task above it. That is 1.75× ([§2.3](#23--elliptic-curve-maths-costs-175-on-core-0)). Both inputs were correct and the subtraction was meaningless.

**What was really there.** No chain-walk overhead at all — the call is 99.8% its two signature verifies ([§2.1](#21--x509_crt_verify-is-its-two-signature-verifies-and-nothing-else)) — and no residual: it was scheduling.

**The rule.** *A primitive benchmark measures a primitive, not a program. Before subtracting one from the other, measure the same call in both contexts and find out what the context costs.*

## Wrong answer 2 — comparing two cells of a matrix that shared no configuration

**The claim.** In the firmware, mbedTLS 4 was level with 3.6.6 on P-256 (612 ms against 601 ms) and **1.83× slower on P-384** — so the regression was concentrated on the curve GitHub's root uses, reversing the spike's earlier "P-384 is not disproportionately affected".

**Why it was wrong.** Every P-384 number in the investigation, harness and firmware, came from the accelerator-**on** configuration, because the harness had only ever run the P-384 benchmarks in the two `mpi-on-nowrap` cells. The tables printed those figures beside P-256 figures drawn from cells with different levers, and nothing in the numbers said so.

**What was really there.** Filling the hole took two builds. In the shipped configuration the regression is a **uniform ~30% on both curves** ([§2.5](#25--the-version-regression-is-a-uniform-30-on-both-curves)), and the compute-only decomposition reaches the same answer independently ([§2.7](#27--the-arduino-builds-advantage-is-code-placement)). The spike's original conclusion was right.

**The rule.** *A benchmark matrix with holes in it will let you compare two cells that share no configuration. Check which cells a number came from before putting it in a table beside another one.*

## Wrong answer 3 — treating one binary's timing as a property of the software

**The claim.** Even like-for-like, the arduino build beat ours on P-384 by a wide margin, and something structural in mbedTLS 4 must explain it.

**Why it was wrong.** It was where the linker put one function. Shifting mbedTLS by 1424 bytes cost the arduino build 20–23% on both curves ([§2.7](#27--the-arduino-builds-advantage-is-code-placement)) and made our build the faster of the two on P-256.

**What was really there.** On a chip executing from flash through a small cache, timing is a property of the *binary*, not the source. This document had already recorded a 13.6% swing from the same cause and still treated a 2× difference as a software property.

**The rule.** *Before explaining a difference between two binaries, perturb the layout of one of them. If the difference moves, it was never about the code.*

## And one that has not been fixed

The **attribution table** for the +2650 ms gap was built entirely from wrong answer 1's arithmetic. It has been left out of this rewrite rather than repaired, because the question it answered — why the native build was slower — no longer has a gap to attribute. If it is ever needed, re-derive it from [§2.3](#23--elliptic-curve-maths-costs-175-on-core-0), [§2.5](#25--the-version-regression-is-a-uniform-30-on-both-curves) and [§2.6](#26--about-half-of-a-verify-is-instruction-fetch).

---

# Part 5 — Working on this

## How to reproduce anything here

**The arduino baseline.**

```
cd D:\source\smolbase-v033
$env:PATH = "$HOME\.platformio\penv\Scripts;$env:PATH"
pio run -e smolbase
pio run -e smolbase -t upload --upload-port COM5
```

Probes log to serial with `[spike]` / `[spike-x509]` / `[spike-psa]`. **The prebuilt `v0.3.3` release binary fails today** (`could not reach GitHub releases`); the source build works. Do not use the release artifact.

**Our build.**

```
& $HOME\esp\esp-idf\export.ps1
cd D:\source\smolbase
idf.py '@smolbase.args' build
idf.py '@smolbase.args' -p COM5 -b 460800 flash
```

To re-enable the handshake timeline add `CONFIG_MBEDTLS_DEBUG=y`, `# CONFIG_MBEDTLS_DEBUG_LEVEL_WARN is not set` and `CONFIG_MBEDTLS_DEBUG_LEVEL_DEBUG=y` to `sdkconfig.defaults`, then **delete `build/smolbase/sdkconfig`** so the defaults take.

**Reading the serial.** A default `SerialPort` drops bytes during a burst and produces plausible-but-wrong text. Use `ReadBufferSize >= 262144` and a tight `ReadExisting()` loop doing nothing else inside it.

**Primitive benchmarks.** `spike/mbedtls-perf/`, with its own [README](../../spike/mbedtls-perf/README.md). `run.ps1 -Runs <tag> -Flash`; captures land in `results/` and collate to `results.csv`. Cells `idf5-mpi-off-fp-nowrap` and `idf6-mpi-off-fp-nowrap` are the shipped levers with no hook, added because the original matrix had P-384 in the accelerator-on cells only. **Flashing the harness erases the Smolbase firmware** — own bootloader and `single_app` partition table. Recover with the full serial flash above. WiFi credentials survive: NVS sits at `0x9000` in both layouts and the harness never writes it.

**Constant-flow testing.**

```
bash spike/mbedtls-perf/upstream/constant-flow/cf_configure.sh
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh revert   # negative control, must FAIL
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh stock    # must PASS
```

**Upstream code style.**

```
bash spike/mbedtls-perf/upstream/constant-flow/build_uncrustify.sh   # 0.75.1, once
cd D:\source\TF-PSA-Crypto
python3 framework/scripts/code_style.py --fix --uncrustify ~/uncrustify-build/uncrustify/build/uncrustify
```

`constant-flow/rebuild_series.sh` rebuilds the whole three-commit series from `development` with the style fix applied per stage.

## Dirty state — revert before the branch ships

| file | what |
|---|---|
| `src/core/Http.cpp` | phase split (open/headers/body), TCP-vs-TLS connect split, the core-0/core-1/core-0 handshake matrix, and the bench calls — `SPIKE` markers throughout |
| `src/core/spike_x509.inc` | the X.509 benchmark, identical source in both builds, plus the library-counter dump |
| `src/core/spike_chain.h` | GitHub's live chain as DER, captured 2026-08-25 |
| `src/core/spike_pad.cpp` | §2.8's placement pad: reachable-never-executed code sized by `SPIKE_PAD_COUNT` (left at 0). Referenced from `Http.cpp` behind a volatile guard |
| `sdkconfig.spike-mpi` | §2.8's config overlay (accelerator on, fixed-point off), used via `SDKCONFIG_DEFAULTS` into `build/smolbase-mpi/` — a scratch build dir, not the shipped one |
| `spike/mbedtls-perf/results/PAD-*` | §2.8's captures: serial logs, symbol snapshots, and the seven ELF/bin images (the binaries are evidence for the placement claims; do not commit them) |
| `src/core/spike_psa_vs_legacy.cpp` | PSA-vs-legacy verify, the core/priority matrix, the per-operand-size multiply benchmark, the `grp.modp` check, the heap counter, and a `--wrap` counter for `mbedtls_mpi_mul_mpi`. Own translation unit because `MBEDTLS_ALLOW_PRIVATE_ACCESS` must precede every mbedTLS include and `Http.cpp` has already pulled in `esp_tls.h`. **The `--wrap` counter is inert unless the build supplies the link flag** |
| `D:\source\smolbase-v033` | the same three probe files plus instrumentation in `src/core/GhUpdate.cpp` and a `--wrap` flag in `platformio.ini`, **all uncommitted**. The device does not run this build — it was restored to Smolbase by full serial flash |

To turn the multiply counter back on, add these two lines to the root `CMakeLists.txt` **after** `include(project.cmake)` and **before** `project(smolbase)` — anywhere else is accepted silently and then fails at link:

```
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=mbedtls_mpi_mul_mpi" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_WRAP_MUL=1" APPEND)
```

Leave the flag out of timing runs: it moves the binary layout and shifted a P-256 verify by 13% with no change to that code path.

**The IDF 6 SDK tree is not pristine.** `library/x509_crt.c` and `tf-psa-crypto/extras/pk_wrap.c` under `~/esp/esp-idf/components/mbedtls/mbedtls` carry the cycle counters from [§2.1](#21--x509_crt_verify-is-its-two-signature-verifies-and-nothing-else); every addition is marked `SPIKE`. Revert with `git checkout` in that tree. The IDF 5.5 tree is untouched. Check `git status` in both before trusting a measurement.

## Traps that cost real time

- **A primitive benchmark is not a program.** See [Part 4](#wrong-answer-1--subtracting-a-benchmark-figure-from-a-firmware-figure). This one cost most of two sessions.
- **Numbers move when the binary changes** — by 13% routinely and by 20%+ when a hot function's placement changes ([§2.7](#27--the-arduino-builds-advantage-is-code-placement)). **Compare only within one build**, and perturb the layout before believing a cross-build difference.
- **Cross-session comparisons are worthless here.** Two conclusions were wrong because a fresh measurement was compared against a number recorded earlier. Measure A-B-A in one session.
- **`idf.py flash` rebuilds.** The tree must be in the intended state at *flash* time, not just build time. A patched cell was built, the patch reverted, and the flash step silently recompiled it unpatched. **Disassemble the flashed ELF and confirm the change is in it.**
- **`xtensa-esp-elf-objdump` sometimes decodes an ELF as raw hex words.** `grep -c call8` then returns 0, indistinguishable from "no calls". A tool returning zero matches is not a tool returning a zero answer.
- **Validate the response body on every timing rep.** Several runs used `curl -o NUL` and measured only elapsed time; a failing request times differently from a succeeding one.
- **`sdkconfig.defaults` cannot override an existing generated `sdkconfig`.** Delete `build/<app>/sdkconfig` after editing it, and assert the value landed before believing a measurement. A Kconfig `choice` also needs the current pick explicitly unset (`# CONFIG_X_40M is not set`) before the alternative takes.
- **Heredocs mangle escape sequences.** `\n` and `\\` inside `python - <<'PY'` have been corrupted repeatedly, once producing a broken C `#define`. Use the Write/Edit tools for anything with escapes.
- **WSL `/tmp` is cleared** when the instance restarts, which happens between tool calls. Use `$HOME`.
- **WSL is behind the same corporate TLS interception as Windows.** Only the device sees GitHub's real chain — dump it from there.
- **`curl` in PowerShell cannot read Git Bash paths** (`/tmp/...`): exit 26, uploads nothing, silently.
- **Changing the flash clock needs a full flash**, not `app-flash` — the frequency lives in the bootloader header.

---

# Open items

**1. Core 0 or core 1 for the handshake.** The only item with a shipping consequence: 3.8 s against 2.1 s. Options and trade-off in [§2.3](#23--elliptic-curve-maths-costs-175-on-core-0). Measure whichever is chosen against the App loop's frame budget, not just against the handshake.

**2. The upstream PR — [Mbed-TLS/TF-PSA-Crypto#873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873), draft. Body updated 2026-08-26** with the findings from §2.5 to §2.7. What went in:

- **P-384**, placed after the O(n)-per-limb paragraph it confirms: 365.26 → 475.03 ms, +30.1%, beside P-256's +33.5% in the same cell. The series had been justified on P-256 alone, and the obvious reviewer question was whether bigger curves fare worse. They do not — the cost is per-limb.
- **Call counts confirmed in firmware**, not just in the harness, via `--wrap` against a precompiled 3.6.6: 5817 and 8569.
- **The flash-cache hypothesis the body flagged as untested is now measured**, and the body says so — the tight-loop-versus-in-verify split, the flash-clock test, and the ~43% against ~24% miss share ([§2.6](#26--about-half-of-a-verify-is-instruction-fetch)).
- **The 875 ms accelerator-on regression is now qualified.** Placement alone moves an ECDSA verify 20–23% on this part ([§2.7](#27--the-arduino-builds-advantage-is-code-placement)) and the series changes code size, so patch effect and placement effect have not been separated. The body now warns reproducers not to expect the same magnitude or sign.
- **Corrected** "the bignum hot path is largely off the code this series touches" — measured, the multiply is 37–41% of a verify, so ~60% is still this series' code.

> **Correction to an earlier note in this document.** That "~875 ms regression the series causes with the accelerator enabled" line was flagged here as a probable transcription error. **It is not.** It is a real A-B-A measurement, four reps each, with the two unpatched runs bracketing the patched one and agreeing to 13 ms, and the PR reported it honestly against its own case. The error was in this document. Read the source before calling something a mistake.

**Second update, same day: the accelerator-on measurement was taken.** The submitted assembly series (not the earlier C variant) was applied to the IDF 6 tree, built into the `idf6-mpi-on-nowrap` harness cell, and run A-B-A — patched, unpatched, patched again — in one session. Every patched image was disassembled before flashing: `mbedtls_mpi_core_sub()` with zero calls and one branch, two calls unpatched.

| accelerator on, core 1 | 3.6.6 | 4.1.0 | with the series | vs 3.6.6 |
|---|---|---|---|---|
| `ecdsa_verify` P-256 | 297.88 ms | 416.99 ms | **296.59 ms** | **−0.4%** |
| `ecdsa_verify` P-384 | 519.25 ms | 738.47 ms | **516.34 ms** | **−0.6%** |
| `ecp_mul` P-256 | 139.55 ms | 195.22 ms | 139.94 ms | +0.3% |
| `ecp_mul` P-384 | 240.20 ms | 341.77 ms | 241.08 ms | +0.4% |
| `mpi_inv_mod` P-256 | 8.67 ms | 12.26 ms | 7.60 ms | −12.4% |

The two patched legs agree to 0.8 µs (296.5844 and 296.5850 ms), and the in-session unpatched figure matches the earlier session's to 0.02%. **So the series reaches parity with 3.6.6 in both accelerator configurations and on both curves** — which also settles what the 875 ms is not. It is not the series computing more slowly; the arithmetic is at parity in exactly that configuration. What is left is code growth or code placement, and those two remain unseparated. Captures: `spike/mbedtls-perf/results/SERIES-{A1,B-unpatched,A2}-idf6-mpi-on-nowrap.log`.

Both PR open items were closed by the padding experiment on 2026-08-29 — see [§2.8](#28--the-875-ms-does-not-survive-the-padding-experiment). In short: the 875 ms does not reproduce (its sign flips — the series is 1.1–2.4 s *faster* end to end in this image family), placement alone moves the patched handshake by ~1 s, and the series **shrinks** Xtensa code: −142 B over the ecp/bignum/constant-time symbols, −144 B whole image. The code-growth hypothesis is dead. The PR body has not yet been updated with these figures.

**3. The Espressif fix — branch pushed, PR not opened, awaiting review.** [`sweetlilmre/esp-idf:fix/mpi-hw-min-bit-len`](https://github.com/sweetlilmre/esp-idf/tree/fix/mpi-hw-min-bit-len), one commit, one file, +100/−0, one ahead of `espressif:master` and zero behind. Description ready in [espressif-hardware-mpi-report-draft.md](espressif-hardware-mpi-report-draft.md); the patch itself is also saved at `spike/mbedtls-perf/upstream/esp-idf-mpi-min-bitlen.patch`.

The fix: `mbedtls_mpi_mul_mpi()` in the port tests an upper size bound and has no lower one, so a 256-bit multiply pays the full peripheral ceremony for a result software produces faster. The branch routes operands below 512 bits to `mbedtls_mpi_core_mul()`. It must test the **real** operand size, not `hw_words` — `mpi_ll_calculate_hardware_words()` rounds up to 16 words, so a 256-bit operand is already presented as 512 bits and the first version of the gate never fired.

Proven, `CONFIG_MBEDTLS_HARDWARE_MPI=y` throughout, all benches `rc=0`:

| | before | after | hw off |
|---|---|---|---|
| `mpi_mul` 256-bit | 19.69 µs | **11.78 µs** | 11.28 µs |
| `ecdsa_verify` P-256 | 416.99 ms | **334.48 ms** | 327.75 ms |
| `mpi_exp_mod` 2048-bit | 19.98 ms | **19.45 ms** | 74.50 ms |
| `mpi_mul` 4096-bit | 460.29 µs | 462.31 µs | 1496.12 µs |
| handshake, core 1 | 4063 ms | **2759 ms** | 2735 ms |

ECC at software speed and RSA still on the hardware, in one build. Captures: `spike/mbedtls-perf/results/THRESHOLD-idf6-mpi-on-nowrap.log`.

**One decision is parked before the PR can be opened:** where the threshold constant should live and whether 512 is safe on parts that were never measured — it could make a chip with a lower crossover slower. Options, recommendation and the measurement that would settle it: [espressif-mpi-threshold-open-question.md](espressif-mpi-threshold-open-question.md). Prior art searched: [espressif/esp-idf#8710](https://github.com/espressif/esp-idf/issues/8710) is the same symptom with no measurement or mechanism, and is closed.

**4. Revert the instrumentation.** The IDF 6 SDK tree, `Http.cpp` and the probe files, and the v0.3.3 worktree. All listed under [*Dirty state*](#dirty-state--revert-before-the-branch-ships).

**5. Fix the benchmark ordering defect** in `mpi_mul_hooked` ([Part 3](#a-harness-defect-the-spike-found-in-itself)). Affects nothing above.
