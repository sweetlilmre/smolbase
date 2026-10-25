# Temporary upstream patches, applied to the SDK tree at configure time.
# See patches/README.md for what they are, the PRs they track, and the revert
# protocol. THIS IS DELIBERATELY TEMPORARY: when a PR merges into the SDK in
# use, its patch must be deleted here and the SDK tree restored.
#
# Semantics per patch, in order:
#   - reverse-applies cleanly  -> already in, skip (idempotent reconfigure)
#   - applies cleanly          -> apply now
#   - neither                  -> FATAL. Either the SDK was upgraded past the
#     merge (drop the patch, see patches/README.md) or something else changed
#     the same lines. Refusing to configure beats building an unknown hybrid.

set(SB_PATCHES ${SB_ROOT}/patches)

function(sb_apply_patch repo patch)
  # ARGN: extra `git apply` arguments (e.g. --directory=..., --include=...).
  execute_process(
    COMMAND git -C ${repo} apply --reverse --check ${ARGN} ${patch}
    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
  if(already EQUAL 0)
    message(STATUS "upstream patch already applied: ${patch}")
    return()
  endif()
  execute_process(
    COMMAND git -C ${repo} apply --check ${ARGN} ${patch}
    RESULT_VARIABLE applies OUTPUT_QUIET ERROR_QUIET)
  if(NOT applies EQUAL 0)
    message(FATAL_ERROR
      "Upstream patch neither applies nor reverse-applies:\n  ${patch}\n"
      "against ${repo}.\n"
      "Most likely the SDK now contains the merged PR - check the PRs in "
      "patches/README.md and follow its revert protocol. Refusing to build "
      "an unknown hybrid.")
  endif()
  execute_process(
    COMMAND git -C ${repo} apply ${ARGN} ${patch}
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git apply failed for ${patch} (rc=${rc})")
  endif()
  message(STATUS "upstream patch APPLIED to SDK tree: ${patch}")
endfunction()

# A patch SERIES has to be tested as a unit: its patches touch the same lines
# (0002 extends 0001's macro list), so once the series is in, patch 1 alone
# neither applies nor reverse-applies and the per-patch check misfires. The
# states are: FIRST patch applies cleanly -> tree unpatched, apply all in
# order; LAST patch reverse-applies cleanly -> series fully in, skip; anything
# else -> the same loud FATAL as sb_apply_patch.
function(sb_apply_series repo)
  # ARGN: dir/strip args first (must start with "--"), then patches in order.
  set(flags "")
  set(patches "")
  foreach(a ${ARGN})
    if(a MATCHES "^-")
      list(APPEND flags ${a})
    else()
      list(APPEND patches ${a})
    endif()
  endforeach()
  list(GET patches 0 first)
  list(GET patches -1 last)
  execute_process(COMMAND git -C ${repo} apply --check ${flags} ${first}
                  RESULT_VARIABLE unpatched OUTPUT_QUIET ERROR_QUIET)
  if(unpatched EQUAL 0)
    foreach(p ${patches})
      execute_process(COMMAND git -C ${repo} apply ${flags} ${p} RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git apply failed mid-series at ${p} (rc=${rc}) - "
                "the SDK tree is now partially patched; restore it with git "
                "checkout there before rebuilding. See patches/README.md.")
      endif()
      message(STATUS "upstream patch APPLIED to SDK tree: ${p}")
    endforeach()
    return()
  endif()
  execute_process(COMMAND git -C ${repo} apply --reverse --check ${flags} ${last}
                  RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
  if(applied EQUAL 0)
    message(STATUS "upstream patch series already applied: ${first} .. ${last}")
    return()
  endif()
  message(FATAL_ERROR
    "Upstream patch series in an unknown state against ${repo}:\n"
    "  first patch does not apply, last does not reverse-apply.\n"
    "Most likely the SDK now contains the merged PR - check the PRs in "
    "patches/README.md and follow its revert protocol. Refusing to build "
    "an unknown hybrid.")
endfunction()

# Mbed-TLS/TF-PSA-Crypto#873 - the constant-time bignum series, in order.
# The mbedTLS submodule is its own git repo; the patches are rooted at
# TF-PSA-Crypto, which the vendored tree carries under tf-psa-crypto/.
set(SB_MBEDTLS_REPO $ENV{IDF_PATH}/components/mbedtls/mbedtls)
sb_apply_series(${SB_MBEDTLS_REPO}
  --directory=tf-psa-crypto -p1
  ${SB_PATCHES}/0001-constant_time-force-inlining-where-an-assembly-path-.patch
  ${SB_PATCHES}/0002-constant_time-add-an-Xtensa-assembly-path.patch
  ${SB_PATCHES}/0003-bignum_core-use-_if_else_0-where-the-zero-branch-is-.patch)

# espressif/esp-idf#19027 - the MPI lower-size threshold, plus the stack-copy
# change its review asked for on 2026-09-04. A SERIES, not independent patches:
# they all edit mpi_mult_mpi_soft(), so once a later one is in the earlier ones
# no longer reverse-apply and a per-patch check would misfire into the FATAL.
#
# All four are generated from the PR branch, so there is no longer a stray
# CMakeLists include-path hunk to filter out and the series takes no --include.
# The FOURTH patch is deliberately not on the PR: it renames the constant to
# ESP_MPI_HW_MIN_BIT_LEN, gives it a per-target/per-clock value, and adds the
# CONFIG_MBEDTLS_MPI_HW_MIN_BIT_LEN override. It touches Kconfig as well as the
# port source. See patches/esp-idf-mpi-kconfig-threshold.patch.
#
# The THIRD patch wipes those stack copies before returning. The heap MPIs the
# second patch replaced were zeroized by mbedtls_mpi_free(), and P-256 field
# multiplies reach this path, so their limbs are secret-dependent. Reported by
# Cursor Bugbot on the PR, 2026-09-08.
#
# It was added, reverted, and reapplied. The revert was my error: it rested on
# a cost measured by comparing two BINARIES, and on this chip layout alone
# moves timings by more than the effect. Re-measured with both arms in one
# binary behind a runtime flag, and with a cycle counter around the wipe block
# so the cost is attributed rather than inferred:
#
#   ECDSA verify P-256   741 us   0.32%     RSA-2048 public   172 us   1.05%
#   ECDSA verify P-384  1122 us   0.22%     RSA-4096 public    57 us   0.09%
#
# At most ~1%, not the ~5% the revert claimed. mbedtls_platform_zeroize() costs
# 0.05 us per call more than a bare memset, so an inlinable replacement would
# buy almost nothing and the library function stays.
#
# The stack copy is worth having on its own account, not just to match the PR.
# Measured 2026-09-11 with a heap arm and a stack arm in ONE binary behind a
# runtime flag, both arms wiping (mbedtls_mpi_free() zeroizes, so the heap arm
# always did), so the delta is purely the allocation:
#
#   ECDSA verify P-256   -8.41%      RSA-2048 public   -15.83%
#   ECDSA verify P-384   -5.88%      RSA-4096 public    -5.77%
#
# Repeat pairs agree to 0.005%. Earlier cross-build figures (-7.4% and -8.9%)
# understated it; RSA-2048 is -15.8%, and RSA-4096 goes from a rounding error
# to -5.77%. Without this patch the threshold patch is a net loss for RSA.
# See docs/research/espressif-mpi-threshold-open-question.md.
sb_apply_series($ENV{IDF_PATH}
  -p1
  ${SB_PATCHES}/esp-idf-mpi-min-bitlen.patch
  ${SB_PATCHES}/esp-idf-mpi-stack-copy.patch
  ${SB_PATCHES}/esp-idf-mpi-zeroize-copy.patch
  ${SB_PATCHES}/esp-idf-mpi-kconfig-threshold.patch)
