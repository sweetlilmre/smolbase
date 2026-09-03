# Temporary upstream patches — REVERT WHEN THE PRs MERGE

These are applied **to the ESP-IDF SDK tree** (not this repo) automatically at CMake configure time by `cmake/upstream_patches.cmake`, so every build gets their benefit while the upstream pull requests are in review. They deliberately leave `$IDF_PATH` dirty — `git status` there will show the patched files, and that is expected while this directory exists.

| patch | applies to | upstream PR | what it buys |
|---|---|---|---|
| `0001-…force-inlining…` + `0002-…Xtensa-assembly-path…` + `0003-…if_else_0…` | `$IDF_PATH/components/mbedtls/mbedtls` (the submodule), `--directory=tf-psa-crypto` | [Mbed-TLS/TF-PSA-Crypto#873](https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873) | the mbedTLS 4 constant-time bignum regression: ~30% off every EC primitive, measured 830 ms (19.8%) off a whole `/api/update/check` in the shipped config |
| `esp-idf-mpi-min-bitlen.patch` | `$IDF_PATH` (only its `esp_bignum.c` hunk is applied) | [espressif/esp-idf#19027](https://github.com/espressif/esp-idf/pull/19027) | routes sub-512-bit multiplies around the RSA peripheral, which is what lets `CONFIG_MBEDTLS_HARDWARE_MPI=y` back into `sdkconfig.defaults`: ECC at software speed, RSA on the hardware (3–4×) |

## The revert protocol

**Check the two PRs above whenever the SDK is upgraded, and periodically.** When a PR merges and the SDK in use contains it:

1. Delete that patch file here (and its entry in `cmake/upstream_patches.cmake` if it is the last of its group).
2. Restore the SDK tree: `git -C $IDF_PATH checkout -- components/mbedtls/port/bignum/esp_bignum.c` and/or `git -C $IDF_PATH/components/mbedtls/mbedtls checkout -- .` as appropriate.
3. If **#19027** is rejected rather than merged, also set `CONFIG_MBEDTLS_HARDWARE_MPI` back to unset in `sdkconfig.defaults` — the accelerator is a net loss for ECC without it.
4. Delete `build/<app>/sdkconfig` and rebuild all three Apps.

The configure step is self-policing about staleness: each patch is applied only if it is not already in (reverse-apply check), and if a patch neither applies nor reverse-applies — which is what happens after an SDK upgrade that includes the merged fix, or a conflicting upstream change — **the configure fails loudly** and points here rather than building an unknown hybrid.

Provenance and evidence: `docs/research/mbedtls4-perf-spike.md` (the whole record), `docs/research/mbedtls4-ct-bignum-root-cause.md` (the series), `docs/research/espressif-mpi-threshold-open-question.md` (the threshold, measured on ESP32 and ESP32-S3).
