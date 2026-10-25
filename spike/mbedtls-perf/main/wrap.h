// Non-invasive call counting for the two bignum entry points the ESP RSA
// accelerator intercepts.
//
// mbedtls_mpi_mul_mpi and mbedtls_mpi_exp_mod are global symbols, so the
// linker's --wrap can count and time every *cross-translation-unit* call
// without patching IDF. That is the direct test of claim C2 (does mbedTLS 4
// call mul_mpi more often per verify than 3.x?) and, together with the
// standalone mul benchmarks, of C3 (does the peripheral's per-call ceremony
// cost more than a software multiply at ECC operand sizes?).
//
// Two caveats that must be carried into any conclusion drawn from these
// numbers:
//
//   1. --wrap only redirects references the linker resolves. Calls made from
//      *inside* the defining translation unit (esp_bignum.c, or bignum.c when
//      the accelerator is off) bind directly and are invisible here. ecp.c is
//      a separate TU, so the ECP multiply path -- the one the stack samples
//      implicated -- is counted.
//   2. Each hook adds two esp_timer_get_time() calls. At 256-bit operands that
//      is a material fraction of the measured time, so bench_wrap_overhead()
//      measures it explicitly and the report prints it.

#pragma once

#include <stdint.h>

#include "compat.h"

// Set from the build (-D SPIKE_NO_WRAP=1) for the uninstrumented control: no
// hooks are linked, every counter reads zero, and spike_real_* are the plain
// symbols. See caveat 2 above for why that control is needed -- the hook's cost
// is not constant across the accelerator lever, so the on-vs-off comparison at
// the ECDSA-verify level is only trustworthy from a no-wrap build.
#ifndef SPIKE_NO_WRAP
#define SPIKE_NO_WRAP 0
#endif

typedef struct {
    uint32_t mul_calls;
    uint32_t exp_calls;
    int64_t mul_us;
    int64_t exp_us;
} spike_counters_t;

void spike_counters_reset(void);
void spike_counters_read(spike_counters_t *out);

// The unwrapped originals, for benchmarks that must not pay the hook's cost.
int spike_real_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B);
int spike_real_exp_mod(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *E,
                       const mbedtls_mpi *N, mbedtls_mpi *prec_RR);
