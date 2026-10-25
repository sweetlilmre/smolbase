// mbedTLS 3.6.6 / 4.1.0 header compatibility for the perf spike.
//
// The spike plan assumed the five benchmarked primitives were public API in
// both versions, so one source file would compile unchanged against each. That
// is NOT true. In mbedTLS 4.1.0 the crypto moved into TF-PSA-Crypto and
// bignum.h, ecp.h and ecdsa.h are all under mbedtls/private/, reachable only
// with MBEDTLS_ALLOW_PRIVATE_ACCESS defined. The *symbols* are unchanged and
// still exported (the ESP port overrides mbedtls_mpi_mul_mpi and
// mbedtls_mpi_exp_mod), so this is purely an include-path and visibility
// problem, and this header is the whole of the fix.
//
// Discriminate on MBEDTLS_VERSION_MAJOR from build_info.h, which exists at the
// same path in both. Do NOT use IDF's MBEDTLS_MAJOR_VERSION compile
// definition: IDF 6 exports it, IDF 5.5 does not.

#pragma once

// Must precede every mbedTLS include. In 4.1.0 this is what un-hides the
// built-in (non-PSA) declarations; in 3.6.6 it also exposes struct members
// directly, which is harmless here and keeps the two builds symmetrical.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "mbedtls/build_info.h"

#if MBEDTLS_VERSION_MAJOR >= 4
#include "mbedtls/private/bignum.h"
#include "mbedtls/private/ecdsa.h"
#include "mbedtls/private/ecp.h"
#else
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#endif

#include "mbedtls/md.h"

// Reported in the identity banner so a measurement can never be attributed to
// the wrong configuration. MBEDTLS_ECP_FIXED_POINT_OPTIM is #defined to 1 or 0
// by esp_config.h rather than defined/undefined, so it must be tested with #if
// and not #ifdef -- getting this wrong silently reports "on" for every build.
#if defined(MBEDTLS_MPI_MUL_MPI_ALT)
#define SPIKE_HW_MUL 1
#else
#define SPIKE_HW_MUL 0
#endif

#if defined(MBEDTLS_MPI_EXP_MOD_ALT)
#define SPIKE_HW_EXP 1
#else
#define SPIKE_HW_EXP 0
#endif

#if MBEDTLS_ECP_FIXED_POINT_OPTIM
#define SPIKE_FIXED_POINT 1
#else
#define SPIKE_FIXED_POINT 0
#endif
