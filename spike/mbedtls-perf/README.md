# mbedTLS perf spike

The harness for [docs/research/mbedtls4-perf-spike.md](../../docs/research/mbedtls4-perf-spike.md). It measures individual mbedTLS primitives on the device, offline, in both mbedTLS 3.6.6 and 4.1.0, with the ESP32's RSA/MPI accelerator on and off.

It exists because the migration produced a working TLS fix whose *reason* is not established. The end-to-end numbers are solid; the mechanism is inference, one part of it already falsified. This measures the mechanism directly.

**It has been run.** All twelve cells are captured in `results/`. **C2 refuted** — both versions make exactly 5817 `mul_mpi` calls per P-256 verify. **C3 confirmed** — 19.49 µs hardware against 9.78 µs software at 256 bits, with the crossover between 256 and 512 bits. **The missing cell says the accelerator was always wrong**: mbedTLS 3.6.6 is 17.8% faster without it too. Full write-up in §8 of the [research doc](../../docs/research/mbedtls4-perf-spike.md).

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
```

### The one thing the spike plan got wrong

The plan assumed all five primitives were public API in both versions, so one source file would compile unchanged. In mbedTLS 4.1.0 the crypto moved into TF-PSA-Crypto and `bignum.h`, `ecp.h` and `ecdsa.h` are all under `mbedtls/private/`, reachable only with `MBEDTLS_ALLOW_PRIVATE_ACCESS` defined. The symbols are unchanged and still exported, so it is purely an include-path and visibility problem — `main/compat.h` is the whole of the fix, discriminating on `MBEDTLS_VERSION_MAJOR` from `build_info.h` (not IDF's `MBEDTLS_MAJOR_VERSION`, which IDF 6 exports and IDF 5.5 does not).

Hashing is deliberately absent: a fixed 32-byte array stands in for the digest, because ECDSA verify does not care whether it is a real hash and not hashing avoids needing a second compat shim for the one API that genuinely changed (`mbedtls_sha256_*` → `psa_hash_*`).
