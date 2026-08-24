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

project(mbedtls_perf)
