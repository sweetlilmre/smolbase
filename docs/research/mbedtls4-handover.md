# mbedTLS 4 TLS performance — handover note

**Written:** 2026-08-25, at the end of the session that measured the arduino-vs-IDF gap properly. Read this first if you are picking this up with no context.
**Branch:** `idf6-migration`, 75 commits ahead of `main`. Working tree clean except the temporary probes listed under *Dirty state* below — those are deliberate and still needed.
**Companions:** [mbedtls4-ct-bignum-root-cause.md](mbedtls4-ct-bignum-root-cause.md) is the full evidence log and the place to add findings · [mbedtls4-perf-spike.md](mbedtls4-perf-spike.md) is the original spike (its §8 conclusions stand, but see *What the spike got wrong*) · [idf6-migration-continuation.md](idf6-migration-continuation.md) is the wider migration state.

## The two open questions

Everything below exists to serve these. Nothing else is outstanding.

1. **Where is the residual 1163 ms?** The handshake gap is +2650 ms and 1487 ms of it is attributed. The unexamined paths are the **key exchange** (ECDHE: server key exchange parse and verify, client key share, shared-secret derivation) and the **record layer** (encryption, MAC, buffer handling). Nothing in this project has isolated either.
2. **Why did `mbedtls_x509_crt_verify` double?** 1270 ms on mbedTLS 3.6.6, 2527 ms on 4.1.0, identical certificate bytes. Its two signature verifies only account for ~1156 ms (4.1.0) and ~817 ms (3.6.6), so several hundred milliseconds of non-signature chain-walk overhead roughly triples between versions. Cause unknown.

## State in one paragraph

The original question — why was the arduino-esp32 build faster than the native IDF build — is now **half answered with measurements rather than inference**. The whole gap is TLS handshake computation: network, HTTP framing, JSON parsing and Kconfig differences are all eliminated by direct measurement. The largest single identified cause is certificate chain verification being twice as slow. A separate strand produced a three-patch series now open as a **draft PR upstream**; it fixes the bignum constant-time regression, which turns out to be a minority of the problem. There is a working, instrumented arduino baseline that can be rebuilt and flashed at will — that capability did not exist before this session and is what made the comparison possible.

## Environment

| | |
|---|---|
| Repo | `D:\source\smolbase`, branch `idf6-migration` |
| ESP-IDF 6.0.2 | `~/esp/esp-idf` → mbedTLS 4.1.0. `& $HOME\esp\esp-idf\export.ps1` |
| ESP-IDF 5.5.5 | `~/esp/esp-idf-v5.5` → mbedTLS 3.6.6. `& $HOME\esp\esp-idf-v5.5\export.ps1` |
| **arduino baseline** | worktree `D:\source\smolbase-v033`, detached at `v0.3.3` (`22f1580`). Builds with `pio` (pioarduino platform, already installed). **Its probes are uncommitted — do not `git checkout` it.** |
| Device | ESP32-D0WD-V3 on **COM5**, RTS auto-reset. Currently running smolbase `0.4.0-dev`, shipped config. IP `10.0.0.32`. |
| Second device | ESP32-S3 on **COM9** — a Waveshare "xiaozhi" board, **not ours**, restored to its original firmware. Leave alone unless a second Xtensa target is needed. |
| Upstream clone | `D:\source\TF-PSA-Crypto`, branch `constant-time-embedded-perf`, `origin` = the fork, `upstream` = Mbed-TLS. Commit identity set locally to `sweetlilmre@gmail.com`. |
| MemSan clone | `~/ctflow/tfpsa` inside WSL Ubuntu — TF-PSA-Crypto `development` with the `framework` submodule, for constant-flow testing. |
| uncrustify 0.75.1 | `~/uncrustify-build/uncrustify/build/uncrustify` in WSL. The version `code_style.py` pins; Ubuntu's 0.78.1 is refused. |

## What is measured, and therefore settled

All same device, same network, same session, same config (RSA/MPI accelerator **ON**, fixed-point off) unless stated.

| | arduino v0.3.3 (3.6.6) | ours (4.1.0) |
|---|---|---|
| TCP connect | ~40 ms | ~40 ms |
| **TLS handshake** | **~3300 ms** | **~5950 ms** |
| body (≈30 KB JSON) | ~197 ms | ~165 ms |

**The entire +2650 ms is handshake computation.** Eliminated by measurement, not argument:

- **Network** — a plain TCP connect to `api.github.com:443` is ~40 ms on both.
- **JSON / the buffered reader** — body read is 111–204 ms; `ClientReader` at `src/core/Http.cpp:31` has its 1436-byte buffer and works.
- **HTTP framing** — headers are 26–60 ms typical.
- **Kconfig** — every performance-critical mbedTLS option is identical in both generated `sdkconfig`s: `ECP_NIST_OPTIM=y`, `ECP_FIXED_POINT_OPTIM` unset, `HARDWARE_MPI=y`, `HARDWARE_SHA`/`AES=y`, `ECDSA_DETERMINISTIC=y`, `ECP_RESTARTABLE` unset, `MPI_USE_INTERRUPT`/`ECP_WINDOW_SIZE`/`ECP_MAX_BITS`/`MPI_WINDOW_SIZE` absent on both. Compare with:
  `diff <(grep -E '^(CONFIG_MBEDTLS|# CONFIG_MBEDTLS)' ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/sdkconfig | sort) <(grep -E '^(CONFIG_MBEDTLS|# CONFIG_MBEDTLS)' build/smolbase/sdkconfig | sort)`

### Attribution of the gap

| cause | ms | how |
|---|---|---|
| `x509_crt_verify` chain walk | **1257** | same DER compiled into both builds, app-level benchmark |
| remaining EC ops (ServerKeyExchange verify + 2 × `ecp_mul`) | 230 | primitive benchmarks |
| **explained** | **1487** | |
| **residual — open question 1** | **1163** | |

### Primitive benchmarks (accelerator ON, same harness build)

| bench | 3.6.6 | 4.1.0 | gap |
|---|---|---|---|
| `ecdsa_verify_p256` | 297.88 ms | 416.92 ms | +40.0% |
| `ecp_mul_p256` | 139.55 ms | 195.19 ms | +39.9% |
| `ecdsa_verify_p384` | 519.25 ms | 738.58 ms | +42.2% |
| `ecp_mul_p384` | 240.20 ms | 341.79 ms | +42.3% |

P-384 is **not** disproportionately affected. Reverting three constant-time lines in `mbedtls_mpi_core_sub`/`_mla` recovers ~100% of this primitive gap in both accelerator configurations — so those lines are the whole primitive story, and the primitives are under half the handshake.

### X.509, identical certificate bytes in both builds

| | arduino 3.6.6 | ours 4.1.0 |
|---|---|---|
| `x509_crt_parse_der` × 3 certs | 8.5 ms | 5.5 ms |
| `x509_crt_verify` | **1270 ms** | **2527 ms** |

Parsing is irrelevant and 4.1.0 is *faster* at it. The doubling is inside the chain walk — **open question 2**.

### Handshake shape (shipped config, from `MBEDTLS_DEBUG` level 3)

`SERVER_CERTIFICATE → SERVER_KEY_EXCHANGE` is 2445 ms, ~58% of the handshake. Within it: 1481 ms before the bundle callback, 5 ms bundle lookup over 146 certs, 949 ms root signature verify (`sig_md=10`, SHA-384, so a P-384 verify). IDF 6's PSA rewrite of `esp_crt_check_signature` accounts for 84 ms of that 949 (949 → 865 with the IDF 5.5 approach).

## How to reproduce anything here

### Flash and measure the arduino baseline

```
cd D:\source\smolbase-v033
$env:PATH = "$HOME\.platformio\penv\Scripts;$env:PATH"
pio run -e smolbase
pio run -e smolbase -t upload --upload-port COM5
```

Its probes log to serial with the `[spike]` / `[spike-x509]` prefix. **The prebuilt `v0.3.3` release binary fails today** (`could not reach GitHub releases`) — the source build works. Do not use the release artifact.

### Flash and measure our build

```
& $HOME\esp\esp-idf\export.ps1
cd D:\source\smolbase
idf.py '@smolbase.args' build
idf.py '@smolbase.args' -p COM5 -b 460800 flash
```

Probes log with the `spike` tag at WARN.

### Read the serial correctly

A default `SerialPort` drops bytes and produces plausible-but-wrong text. Use `ReadBufferSize >= 262144` and a tight `ReadExisting()` loop doing nothing else. Pattern used throughout this session is in the git history of this branch.

### Primitive benchmarks

`spike/mbedtls-perf/` with its own [README](../../spike/mbedtls-perf/README.md). `run.ps1 -Runs <tag> -Flash`. Twelve-plus cells across both SDKs; captures in `results/`. **Flashing it erases the smolbase firmware** (own bootloader and partition table); recover with the full serial flash above. In practice WiFi credentials survive, because NVS sits at `0x9000` in both layouts and the spike never writes it.

### Constant-flow testing

```
bash spike/mbedtls-perf/upstream/constant-flow/cf_configure.sh
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh revert   # negative control, must FAIL
bash spike/mbedtls-perf/upstream/constant-flow/cf_prove.sh stock    # must PASS
```

### Upstream code style

```
bash spike/mbedtls-perf/upstream/constant-flow/build_uncrustify.sh   # 0.75.1, once
cd D:\source\TF-PSA-Crypto
python3 framework/scripts/code_style.py --fix --uncrustify ~/uncrustify-build/uncrustify/build/uncrustify
```

`spike/mbedtls-perf/upstream/constant-flow/rebuild_series.sh` rebuilds the whole three-commit series from `development` with the style fix applied per stage.

## Dirty state that is deliberate — and must be reverted before the branch ships

| file | what |
|---|---|
| `src/core/Http.cpp` | phase split (open/headers/body), TCP-vs-TLS connect split, `spike_x509_bench()` call — 15 `SPIKE` markers |
| `src/core/spike_x509.inc` | the X.509 benchmark, identical source in both builds |
| `src/core/spike_chain.h` | GitHub's live chain as DER, captured 2026-08-25 |
| `D:\source\smolbase-v033` | the same two files plus instrumentation in `src/core/GhUpdate.cpp`, **uncommitted** |

Both SDK trees (`~/esp/esp-idf`, `~/esp/esp-idf-v5.5`) are **pristine** — always check `git status` in `~/esp/esp-idf/components/mbedtls/mbedtls` before trusting a measurement.

## Traps that cost real time this session

- **`idf.py flash` rebuilds.** The source tree must be in the intended state at *flash* time, not just build time. A patched cell was built, the patch reverted, and the flash step silently recompiled it unpatched — the numbers came back byte-identical to the unpatched run, which is the only reason it was caught. **Always disassemble the flashed ELF and confirm the change is in it.**
- **`xtensa-esp-elf-objdump` sometimes decodes an ELF as raw hex words.** `grep -c call8` then returns 0, which looks exactly like "no calls". A tool returning zero matches is not a tool returning a zero answer. Cross-check with binary size or another signal.
- **Cross-session comparisons are worthless here.** Two conclusions in this session were wrong because a fresh measurement was compared against a number recorded earlier. Always measure A-B-A in one session.
- **Timing runs must validate the response body.** Several runs used `curl -o NUL` and measured only elapsed time; a failing request times differently from a succeeding one. Check for `"latest"` in the body every rep.
- **Numbers move when the binary changes.** Adding P-384 benchmarks shifted the *P-256* result on 4.1.0 by 13.6% with no change to that code path. Flash-cache pressure. Compare only within one build.
- **Heredocs mangle escape sequences.** `\n` and `\\` in a `python - <<'PY'` block have been corrupted repeatedly, once producing a broken C `#define` and once a broken generator script. Use the Write/Edit tools for anything with escapes.
- **`/tmp` in WSL is cleared** when the instance restarts, which happens between tool calls. Use `$HOME`.
- **WSL is behind the same corporate TLS interception as Windows.** `openssl s_client` from either shows Netskope's chain, not the real one. Only the device sees GitHub's real chain — dump it from there.
- **`curl` in PowerShell cannot read Git Bash paths** (`/tmp/...`). Exit 26, silently uploads nothing.

## What the spike got wrong, and why that matters for question 2

The spike measured bignum primitives exhaustively and never asked what fraction of a handshake they are. They are under half. The same failure mode is the risk for both open questions: **measure the share before optimising the part**. The phase split in `core/Http.cpp` and the `MBEDTLS_DEBUG` timeline are the tools for doing that — use them to size a component before benchmarking it.

## Suggested attack on the two open questions

**Question 2 (chain walk doubled) is the more tractable and the more valuable.** Both builds can run the identical `spike_x509_bench()` over identical DER, so it is a clean A/B with no network involved. Take `mbedtls_x509_crt_verify` apart: it does two signature verifies, and the primitives say those are ~1156 ms of the 2527. The remaining ~1370 ms is chain-walk overhead. Candidates never examined: `mbedtls_pk_parse_public_key` per level, the PSA key-import path that mbedTLS 4 routes `mbedtls_pk_verify` through (the same mechanism already measured at 84 ms in `esp_crt_check_signature`, but here it would be per certificate), name/constraint checking, and `x509_crt_check_signature`'s hash step. Our side can be instrumented directly in `~/esp/esp-idf/components/mbedtls/mbedtls`; the arduino side cannot, so anything needing a per-build comparison has to go through the public API from application code, as `spike_x509.inc` does.

**Question 1 (residual 1163 ms)** needs the handshake sized before it is benchmarked. Re-enable `MBEDTLS_DEBUG` (`CONFIG_MBEDTLS_DEBUG=y` plus `# CONFIG_MBEDTLS_DEBUG_LEVEL_WARN is not set` and `CONFIG_MBEDTLS_DEBUG_LEVEL_DEBUG=y`, and delete `build/smolbase/sdkconfig` so the defaults take) and read the client-state timeline. In the shipped config the two non-certificate blocks were `SERVER_KEY_EXCHANGE → CERTIFICATE_REQUEST` at 520 ms and `CLIENT_KEY_EXCHANGE → CERTIFICATE_VERIFY` at 785 ms; both are ECDHE work and neither has been compared against the arduino build. There is no equivalent timeline available on the arduino side, so the comparison again has to be an application-level benchmark of the same operation — `mbedtls_ecdh_*` over fixed inputs would be the analogue of what `spike_x509.inc` does for certificates.

## Upstream PR — separate strand, do not conflate

**[Mbed-TLS/TF-PSA-Crypto#873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873), draft.** Three commits fixing the bignum constant-time regression: forced inlining where an asm path exists, a new Xtensa asm path, and `_if_else_0` at two call sites. CI code style passes. The body carries the measurements, the constant-flow results with a working negative control, an explicit not-verified list, and the measured ~875 ms **regression** the series causes on ESP32 builds with the RSA/MPI accelerator enabled.

It fixes the primitives. The primitives are under half the problem. Findings from the two open questions above are likely to be **more** valuable upstream than what is currently in that PR, and should probably be reported separately rather than folded into it.
