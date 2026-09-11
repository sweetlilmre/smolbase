# mbedTLS perf spike. Builds UNCHANGED against two SDKs:
#
#   ESP-IDF v6.0.2  -> mbedTLS 4.1.0 (TF-PSA-Crypto)   ~/esp/esp-idf
#   ESP-IDF v5.5.5  -> mbedTLS 3.6.6                   ~/esp/esp-idf-v5.5
#
# Whichever export script is sourced picks the version; there is nothing here to
# switch. main/compat.h absorbs the one real difference (bignum/ecp/ecdsa moved
# under mbedtls/private/ in 4.x).
#
# The four runs the spike needs, each in its own build directory so no generated
# sdkconfig is ever reused across configurations -- sdkconfig.defaults cannot
# override an existing sdkconfig, and that trap has produced silently wrong
# measurements in this project before:
#
#   idf.py '@mpi-on.args'      build   # accelerator on,  fixed-point off
#   idf.py '@mpi-off.args'     build   # accelerator off, fixed-point off
#   idf.py '@mpi-on-fp.args'   build   # accelerator on,  fixed-point on
#   idf.py '@mpi-off-fp.args'  build   # accelerator off, fixed-point on
#
# Run each under both SDKs for the eight-cell matrix. Cell (3.6.6, accelerator
# off) is the one the original work could not reach at all, because the Arduino
# mbedTLS was precompiled.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Count and time every cross-TU call to the two symbols the ESP RSA accelerator
# overrides. A build property rather than a component target property, so it
# lands on the final executable link in both IDF 5.5 and 6.0 -- and it MUST come
# before project(), which is where the link command is assembled. Setting it
# after project() is accepted silently and then fails at link with "undefined
# reference to __real_mbedtls_mpi_mul_mpi", which is the good failure mode; the
# bad one is a --wrap that resolves nothing and reports zero calls, which
# main/bench.c checks for explicitly.
#
# -D SPIKE_NO_WRAP=1 omits the hooks entirely. It exists because the hook turned
# out to cost 2.33 us per call with the accelerator off but 11.08 us with it on,
# and an ECDSA verify makes thousands of hooked calls -- so the instrumentation
# is the same order as the accelerator effect the verify rows are meant to
# measure, and the on-vs-off comparison needs an uninstrumented control. Call
# counts and the unhooked mul sweep are unaffected either way.
if(SPIKE_NO_WRAP)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_NO_WRAP=1" APPEND)
else()
  idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=mbedtls_mpi_mul_mpi" APPEND)
  idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=mbedtls_mpi_exp_mod" APPEND)
endif()

# -D SPIKE_MPI_THRESHOLD=<bits> overrides SOC_MPI_HW_MIN_BIT_LEN across the whole
# build, the SDK's own esp_bignum.c included -- its definition of that constant is
# #ifndef-guarded precisely so it can be overridden. A build property, not a
# component one, because the constant has to reach the mbedtls component and not
# just this app.
#
# The crossover cells pass 0: with the routing threshold at zero every multiply
# reaches the peripheral, which gives the hardware column, while main/bench.c
# calls the software path directly for the software column. One binary, both
# curves, no layout difference between them.
if(DEFINED SPIKE_MPI_THRESHOLD)
  idf_build_set_property(COMPILE_DEFINITIONS "SOC_MPI_HW_MIN_BIT_LEN=${SPIKE_MPI_THRESHOLD}" APPEND)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_MPI_THRESHOLD=${SPIKE_MPI_THRESHOLD}" APPEND)
endif()

# -D SPIKE_CROSSOVER=1 adds the 32-bit-step 256..512 sweep, in both hardware and
# software, to answer where the crossover really sits. Off by default: it roughly
# doubles the run time and every existing capture in results/ was taken without
# it, so the default keeps those comparable.
if(SPIKE_CROSSOVER)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_CROSSOVER=1" APPEND)
endif()

# -D SPIKE_MULSTEP=1 moves that sweep up an octave (15..25 limbs) and drops the
# hardware column, to test whether the software cost is a sawtooth in limb count.
# Mutually exclusive with SPIKE_CROSSOVER: they share one operand array and one
# size list, and asking for both would silently measure the second one's sizes
# under the first one's bench names.
if(SPIKE_MULSTEP)
  if(SPIKE_CROSSOVER)
    message(FATAL_ERROR "SPIKE_CROSSOVER and SPIKE_MULSTEP are mutually exclusive - they share XBITS.")
  endif()
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_MULSTEP=1" APPEND)
endif()

# -D SPIKE_SOFTCOUNT=1 reads a counter injected into the SDK's esp_bignum.c by
# the fix-stackcopy-count variant, and reports entries to mpi_mult_mpi_soft()
# per operation. It exists because -Wl,--wrap cannot see same-TU calls, so the
# wrap counter reports zero mul_mpi calls for exp_mod and that cannot be taken
# at face value.
if(SPIKE_SOFTCOUNT)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_SOFTCOUNT=1" APPEND)
endif()

# -D SPIKE_WIPEAB=1 pairs each benchmark with a wipe-on and a wipe-off arm,
# selected by a runtime flag in the SDK, and adds a direct microbenchmark of
# mbedtls_platform_zeroize against memset. Needs the fix-stackcopy-ab variant,
# which is what defines the flag.
if(SPIKE_WIPEAB)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_WIPEAB=1" APPEND)
endif()

# -D SPIKE_HEAPAB=1 pairs a heap-copy arm with a stack-copy arm, chosen at run
# time, to re-measure what the stack copy actually buys. Needs fix-heapab.
if(SPIKE_HEAPAB)
  idf_build_set_property(COMPILE_DEFINITIONS "SPIKE_HEAPAB=1" APPEND)
endif()

project(mbedtls_perf)
