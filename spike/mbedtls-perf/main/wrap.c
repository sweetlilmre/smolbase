#include "wrap.h"

#include "esp_timer.h"

// Provided by the linker because of -Wl,--wrap in the project CMakeLists.
int __real_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B);
int __real_mbedtls_mpi_exp_mod(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *E,
                              const mbedtls_mpi *N, mbedtls_mpi *prec_RR);

// Not volatile and not atomic: the benchmark task is the only writer, it is
// pinned to one core, and nothing else in this app touches mbedTLS.
static spike_counters_t s_c;

int __wrap_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B)
{
    const int64_t t0 = esp_timer_get_time();
    const int rc = __real_mbedtls_mpi_mul_mpi(X, A, B);
    s_c.mul_us += esp_timer_get_time() - t0;
    s_c.mul_calls++;
    return rc;
}

int __wrap_mbedtls_mpi_exp_mod(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *E,
                               const mbedtls_mpi *N, mbedtls_mpi *prec_RR)
{
    const int64_t t0 = esp_timer_get_time();
    const int rc = __real_mbedtls_mpi_exp_mod(X, A, E, N, prec_RR);
    s_c.exp_us += esp_timer_get_time() - t0;
    s_c.exp_calls++;
    return rc;
}

void spike_counters_reset(void)
{
    s_c = (spike_counters_t){0};
}

void spike_counters_read(spike_counters_t *out)
{
    *out = s_c;
}

// A benchmark that wants the primitive's own cost, not the primitive plus two
// esp_timer reads, calls through these. They also let bench.c quantify the
// hook's overhead by timing the same operation both ways.
int spike_real_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B)
{
    return __real_mbedtls_mpi_mul_mpi(X, A, B);
}

int spike_real_exp_mod(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *E,
                      const mbedtls_mpi *N, mbedtls_mpi *prec_RR)
{
    return __real_mbedtls_mpi_exp_mod(X, A, E, N, prec_RR);
}
