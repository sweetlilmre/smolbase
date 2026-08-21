# 0005 — Rebuild the IDF libs locally (hybrid compile) for software-MPI RSA and a 4 KB TLS TX buffer

**Status**: accepted (2026-08-21)

## Context

pioarduino normally links arduino-esp32 against **precompiled** ESP-IDF static libraries. Everything baked into those `.a` files — mbedTLS buffer sizes, hardware-crypto routing — is unreachable from `build_flags`; `-D` redefines are ignored or ODR-hazardous (see `docs/research/ghupdate-tls-knobs.md`).

Two baked-in settings became real problems (wayfinder map #112, tickets #115/#119):

1. `CONFIG_MBEDTLS_HARDWARE_MPI=y` with `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI` **unset**. The ESP32 RSA accelerator cannot complete 4096-bit signature verifies, and the stock libs compile in no software fallback. In August 2026 GitHub moved release assets to a CDN chain cross-signed by the RSA-4096 ISRG Root X1 — PK verify fails with `0x4290` (`RSA_PUBLIC_FAILED | MPI_ALLOC_FAILED`) regardless of free heap, and the GitHub OTA died in the field. Any future peer with a ≥4096-bit link in its chain would hit the same wall.
2. `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` symmetric: two 16 KB TLS I/O buffers per connection (~32 KB), although this firmware only ever *sends* tiny requests.

## Decision

Use pioarduino's **hybrid compile**: a `custom_sdkconfig` block in `platformio.ini` makes the platform rebuild the IDF static libs locally with:

- `CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI=y` — software bignum fallback for RSA ops beyond the accelerator (≈1–2 s CPU per affected handshake; correctness over speed).
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` + `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` — TX buffer 16 KB → 4 KB, ~12 KB less heap per TLS connection, for every TLS user in the firmware.

**The RX buffer stays 16384.** GitHub's CDN sends full-size TLS records, the client advertises neither MFL nor RFC 8449 `record_size_limit`, and an undersized RX buffer fails hard with `MBEDTLS_ERR_SSL_INVALID_RECORD` mid-stream.

## Consequences

- The first build (per `platformio.ini` change) recompiles the IDF libs: ~15 min locally, ~10 min in CI. Locally the rebuild is skipped via a hash stamped into the generated `sdkconfig.defaults`; in CI the `.pio`/`sdkconfig.defaults` cache (keyed on `platformio.ini`'s hash) does the same. Tag-release builds read main's cache, so releases stay warm after the first main build.
- On Windows, builds must run from **PowerShell/cmd** — ESP-IDF's `idf_tools.py` refuses MSYS/git-bash environments outright.
- Generated files appear in the project root (`sdkconfig.defaults`, `sdkconfig.<env>`, `managed_components/`) — gitignored, regenerated per build. The hybrid lib phase also prints a spurious `src_filter` warning (documented in `platformio.ini`).
- The rebuilt libs are copied into the shared `~/.platformio` framework package; building a branch *without* `custom_sdkconfig` triggers a stock-package reinstall, so switching between such branches costs a few minutes each way.
- Reverting the `custom_sdkconfig` commit alone restores stock precompiled libs — and reintroduces the 4096-bit verify failure.
