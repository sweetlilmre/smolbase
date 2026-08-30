// mbedTLS 4.1.0 vs 3.6.6 primitive benchmark — the spike specified in
// docs/research/mbedtls4-perf-spike.md.
//
// Everything that made the original wall-clock measurements uninterpretable is
// gone by construction: no WiFi, no TLS, no HTTP, no JSON, no display, no touch
// filter, no network peer. Fixed operands, a fixed private key, one task pinned
// to core 1, esp_timer as the clock. What is left is the cost of the primitives
// themselves, in both mbedTLS versions, with the RSA/MPI accelerator on and off.
//
// The two questions it exists to answer:
//
//   C2  Does mbedTLS 4's ECP call mbedtls_mpi_mul_mpi materially more often per
//       ECDSA verify than 3.6.6 does? (mul_calls_per_op below.)
//   C3  At ECC operand sizes, does the accelerator's per-call ceremony cost more
//       than a software multiply? (the mpi_mul size sweep, run with the
//       accelerator on and off.)
//
// The mul sweep runs 256 -> 4096 bits deliberately: if C3 holds there is a
// crossover size above which the peripheral wins, and the ESP port applies no
// lower size threshold, so the crossover is the whole story.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compat.h"
#include "wrap.h"

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// ---- Timing policy ------------------------------------------------------
// A batch is sized at run time to about BATCH_TARGET_US so every benchmark is
// timed over a comparable interval regardless of how expensive one operation
// is: 4096-bit exp_mod ends up at one iteration per batch, a 256-bit multiply
// at thousands. Reporting min/mean/max over REPS batches (rather than a single
// mean) is what makes a scheduler hiccup visible instead of averaged in.
#define BATCH_TARGET_US 200000
#define MAX_ITERS 200000
#define REPS 5

// RFC 6979 A.2.5's P-256 private key. Used because it is a published,
// unambiguous scalar known to be valid for this curve, not because any RFC 6979
// answer is checked here.
#define D_HEX "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721"

// RFC 6979 A.2.6's P-384 private key, for the same reason as D_HEX above:
// a published, unambiguous scalar known to be valid for this curve.
#define D384_HEX "6B9D3DAD2E1B8C1C05B19875B6659F4DE23C3B667BF297BA9AA47740787137D896D5724E4C70A825F872C9EA60D2EDF5"

// A fixed 32-byte "digest". ECDSA verify does not care whether this is the
// output of a real hash, and not hashing keeps the harness free of the one API
// that genuinely did change between the two versions (mbedtls_sha256_* ->
// psa_hash_*), which would otherwise need its own compat shim.
static const unsigned char HASH32[32] = {
    0xaf, 0x2b, 0xdb, 0xe1, 0xaa, 0x9b, 0x6e, 0xc1, 0xe2, 0xad, 0xe1, 0xd6, 0x94, 0xf4, 0x1f, 0xc7,
    0x1a, 0x83, 0x1d, 0x02, 0x68, 0xe9, 0x89, 0x15, 0x62, 0x11, 0x3d, 0x8a, 0x62, 0xad, 0xd1, 0xbf,
};

// A fixed 48-byte "digest" for the P-384 benchmarks, for the same reason as
// HASH32: ECDSA does not care whether it is a real hash.
static const unsigned char HASH48[48] = {
    0x9a, 0x9d, 0xd5, 0x1f, 0xe9, 0x50, 0x92, 0x2a, 0xed, 0x1e, 0x8a, 0x2b,
    0x3b, 0x0d, 0x6a, 0x4c, 0x37, 0x77, 0x59, 0x18, 0x2b, 0xc1, 0x2a, 0x0e,
    0x9d, 0x4c, 0x71, 0x2d, 0x6a, 0x1f, 0x53, 0x84, 0x2f, 0xd6, 0x8b, 0x77,
    0x1c, 0x39, 0x0e, 0x55, 0x74, 0x2b, 0x9c, 0x11, 0x63, 0x8e, 0x0a, 0x52,
};

// ---- Deterministic RNG --------------------------------------------------
// mbedTLS wants an RNG for signature blinding and for the blinded point
// multiply. A real entropy source would make the two builds do different work
// on the same inputs, so this is a fixed-seed xorshift, re-seeded before every
// use. Not secure, and not pretending to be: it exists so that "the same
// operand" means the same thing in both runs.
typedef struct {
    uint32_t s;
} prng_t;

static int prng_fill(void *ctx, unsigned char *out, size_t len)
{
    prng_t *p = (prng_t *)ctx;
    for (size_t i = 0; i < len; i++) {
        p->s ^= p->s << 13;
        p->s ^= p->s >> 17;
        p->s ^= p->s << 5;
        out[i] = (unsigned char)(p->s & 0xff);
    }
    return 0;
}

static prng_t g_prng;

static void prng_seed(uint32_t seed)
{
    g_prng.s = seed ? seed : 1u;
}

// ---- Fixed operands -----------------------------------------------------
static int mpi_fixed(mbedtls_mpi *X, size_t bits, uint32_t seed, int force_odd)
{
    unsigned char buf[512]; // 4096 bits
    const size_t len = bits / 8;
    if (len > sizeof(buf)) {
        return -1;
    }
    prng_t p = {.s = seed};
    prng_fill(&p, buf, len);
    buf[0] |= 0x80;         // exactly `bits` wide, so the size sweep is honest
    if (force_odd) {
        buf[len - 1] |= 0x01; // exp_mod requires an odd modulus
    }
    return mbedtls_mpi_read_binary(X, buf, len);
}

// ---- Benchmark state ----------------------------------------------------
static mbedtls_ecp_group g_grp;
static mbedtls_ecp_point g_Q;   // public key, = d*G
static mbedtls_ecp_point g_R;   // scratch output for ecp_mul
static mbedtls_mpi g_d, g_r, g_s;

// P-384. Added because GitHub's certificate chain is verified with a
// P-384 root (ECDSA-SHA384), and that verify is the single most expensive
// operation in the handshake -- while every other benchmark here is P-256.
static mbedtls_ecp_group g_grp384;
static mbedtls_ecp_point g_Q384, g_R384;
static mbedtls_mpi g_d384, g_r384, g_s384;

// 384 appended (index 5, out of size order) so the original five keep their
// indices and every earlier capture stays comparable. Added for the S3
// crossover measurement: the esp-idf threshold fix gates at 512 bits, and 384
// is the largest ECC operand below the gate.
static mbedtls_mpi g_mul_a[6], g_mul_b[6], g_mul_x; // 256/512/1024/2048/4096/384
static const size_t MUL_BITS[6] = {256, 512, 1024, 2048, 4096, 384};

static mbedtls_mpi g_exp2_a, g_exp2_n, g_exp4_a, g_exp4_n, g_exp_e, g_exp_x;
static mbedtls_mpi g_inv_a, g_inv_x;

static int g_last_rc; // non-zero here means a benchmark silently failed

// ---- The operations -----------------------------------------------------
// Each is exactly one primitive call, so the batch loop measures the primitive
// and nothing else. Return codes are accumulated rather than checked per call:
// a benchmark that errors out fast would otherwise look like a fast benchmark.

static void op_ecdsa_verify(void)
{
    g_last_rc |= mbedtls_ecdsa_verify(&g_grp, HASH32, sizeof(HASH32), &g_Q, &g_r, &g_s);
}

static void op_ecdsa_verify_384(void)
{
    g_last_rc |= mbedtls_ecdsa_verify(&g_grp384, HASH48, sizeof(HASH48), &g_Q384,
                                      &g_r384, &g_s384);
}

static void op_ecp_mul_384(void)
{
    prng_seed(0x5eed4321u);
    g_last_rc |= mbedtls_ecp_mul(&g_grp384, &g_R384, &g_d384, &g_grp384.G,
                                 prng_fill, &g_prng);
}

static void op_ecp_mul(void)
{
    prng_seed(0x5eed1234u);
    g_last_rc |= mbedtls_ecp_mul(&g_grp, &g_R, &g_d, &g_grp.G, prng_fill, &g_prng);
}

// The size sweep goes through spike_real_mul_mpi, not the wrapped symbol: the
// hook's two esp_timer reads are a material fraction of a 256-bit multiply and
// would be indistinguishable from the accelerator's own overhead, which is the
// exact quantity under test.
static void op_mul_256(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[0], &g_mul_b[0]); }
static void op_mul_384(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[5], &g_mul_b[5]); }
static void op_mul_512(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[1], &g_mul_b[1]); }
static void op_mul_1024(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[2], &g_mul_b[2]); }
static void op_mul_2048(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[3], &g_mul_b[3]); }
static void op_mul_4096(void) { g_last_rc |= spike_real_mul_mpi(&g_mul_x, &g_mul_a[4], &g_mul_b[4]); }

// Same 256-bit multiply, through the wrapper. mean(this) - mean(op_mul_256) is
// the hook's cost per call, which is what licenses reading mul_ns_per_op below
// as a real number rather than an artefact.
static void op_mul_256_hooked(void)
{
    g_last_rc |= mbedtls_mpi_mul_mpi(&g_mul_x, &g_mul_a[0], &g_mul_b[0]);
}

static void op_exp_2048(void)
{
    g_last_rc |= spike_real_exp_mod(&g_exp_x, &g_exp2_a, &g_exp_e, &g_exp2_n, NULL);
}

// The #119 operation: the 4096-bit modular exponentiation in GitHub's ISRG Root
// X1 cross-signed CDN chain, which the accelerator could not complete.
static void op_exp_4096(void)
{
    g_last_rc |= spike_real_exp_mod(&g_exp_x, &g_exp4_a, &g_exp_e, &g_exp4_n, NULL);
}

static void op_inv_256(void)
{
    g_last_rc |= mbedtls_mpi_inv_mod(&g_inv_x, &g_inv_a, &g_grp.P);
}

typedef struct {
    const char *name;
    unsigned bits;
    void (*fn)(void);
} bench_t;

static const bench_t BENCHES[] = {
    {"ecdsa_verify_p256", 256, op_ecdsa_verify},
    {"ecp_mul_p256", 256, op_ecp_mul},
    {"ecdsa_verify_p384", 384, op_ecdsa_verify_384},
    {"ecp_mul_p384", 384, op_ecp_mul_384},
    {"mpi_inv_mod_p256", 256, op_inv_256},
    {"mpi_mul", 256, op_mul_256},
    {"mpi_mul", 384, op_mul_384},
    {"mpi_mul", 512, op_mul_512},
    {"mpi_mul", 1024, op_mul_1024},
    {"mpi_mul", 2048, op_mul_2048},
    {"mpi_mul", 4096, op_mul_4096},
    {"mpi_mul_hooked", 256, op_mul_256_hooked},
    {"mpi_exp_mod_e65537", 2048, op_exp_2048},
    {"mpi_exp_mod_e65537", 4096, op_exp_4096},
};

// ---- Setup --------------------------------------------------------------
static int setup(void)
{
    int rc;

    mbedtls_ecp_group_init(&g_grp);
    mbedtls_ecp_point_init(&g_Q);
    mbedtls_ecp_point_init(&g_R);
    mbedtls_mpi_init(&g_d);
    mbedtls_mpi_init(&g_r);
    mbedtls_mpi_init(&g_s);
    for (int i = 0; i < 6; i++) {
        mbedtls_mpi_init(&g_mul_a[i]);
        mbedtls_mpi_init(&g_mul_b[i]);
    }
    mbedtls_mpi_init(&g_mul_x);
    mbedtls_mpi_init(&g_exp2_a);
    mbedtls_mpi_init(&g_exp2_n);
    mbedtls_mpi_init(&g_exp4_a);
    mbedtls_mpi_init(&g_exp4_n);
    mbedtls_mpi_init(&g_exp_e);
    mbedtls_mpi_init(&g_exp_x);
    mbedtls_mpi_init(&g_inv_a);
    mbedtls_mpi_init(&g_inv_x);

    if ((rc = mbedtls_ecp_group_load(&g_grp, MBEDTLS_ECP_DP_SECP256R1)) != 0) {
        printf("[error] ecp_group_load rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }
    if ((rc = mbedtls_mpi_read_string(&g_d, 16, D_HEX)) != 0) {
        printf("[error] read d rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    prng_seed(0xa5a5a5a5u);
    if ((rc = mbedtls_ecp_mul(&g_grp, &g_Q, &g_d, &g_grp.G, prng_fill, &g_prng)) != 0) {
        printf("[error] ecp_mul (public key) rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    prng_seed(0xdeadbeefu);
#if defined(MBEDTLS_ECDSA_DETERMINISTIC)
    // RFC 6979. The signature is a function of (d, hash) alone, so it is
    // identical in both versions and the r/s printed below are a direct check
    // that the two runs verified the same thing.
    rc = mbedtls_ecdsa_sign_det_ext(&g_grp, &g_r, &g_s, &g_d, HASH32, sizeof(HASH32),
                                    MBEDTLS_MD_SHA256, prng_fill, &g_prng);
#else
    // Randomised ECDSA, but with the fixed-seed PRNG above, so still repeatable
    // within a version. It may differ BETWEEN versions if they consume RNG
    // bytes differently, which is why r/s is printed rather than assumed.
    rc = mbedtls_ecdsa_sign(&g_grp, &g_r, &g_s, &g_d, HASH32, sizeof(HASH32), prng_fill, &g_prng);
#endif
    if (rc != 0) {
        printf("[error] ecdsa_sign rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    // Refuse to benchmark a verify that does not verify. A failing verify can
    // return early and would time as a suspiciously fast one -- exactly the
    // "pass condition that accepts too little evidence" this spike exists to
    // avoid.
    if ((rc = mbedtls_ecdsa_verify(&g_grp, HASH32, sizeof(HASH32), &g_Q, &g_r, &g_s)) != 0) {
        printf("[error] self-check verify rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    // ---- P-384, mirroring the P-256 setup above exactly ----
    mbedtls_ecp_group_init(&g_grp384);
    mbedtls_ecp_point_init(&g_Q384);
    mbedtls_ecp_point_init(&g_R384);
    mbedtls_mpi_init(&g_d384);
    mbedtls_mpi_init(&g_r384);
    mbedtls_mpi_init(&g_s384);

    if ((rc = mbedtls_ecp_group_load(&g_grp384, MBEDTLS_ECP_DP_SECP384R1)) != 0) {
        printf("[error] ecp_group_load p384 rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }
    if ((rc = mbedtls_mpi_read_string(&g_d384, 16, D384_HEX)) != 0) {
        printf("[error] read d384 rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }
    prng_seed(0xb6b6b6b6u);
    if ((rc = mbedtls_ecp_mul(&g_grp384, &g_Q384, &g_d384, &g_grp384.G,
                              prng_fill, &g_prng)) != 0) {
        printf("[error] ecp_mul p384 (public key) rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }
    prng_seed(0xfeedfaceu);
#if defined(MBEDTLS_ECDSA_DETERMINISTIC)
    rc = mbedtls_ecdsa_sign_det_ext(&g_grp384, &g_r384, &g_s384, &g_d384,
                                    HASH48, sizeof(HASH48), MBEDTLS_MD_SHA384,
                                    prng_fill, &g_prng);
#else
    rc = mbedtls_ecdsa_sign(&g_grp384, &g_r384, &g_s384, &g_d384,
                            HASH48, sizeof(HASH48), prng_fill, &g_prng);
#endif
    if (rc != 0) {
        printf("[error] ecdsa_sign p384 rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }
    // Same gate as P-256: refuse to benchmark a verify that does not verify.
    if ((rc = mbedtls_ecdsa_verify(&g_grp384, HASH48, sizeof(HASH48), &g_Q384,
                                   &g_r384, &g_s384)) != 0) {
        printf("[error] self-check verify p384 rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    for (int i = 0; i < 6; i++) {
        if ((rc = mpi_fixed(&g_mul_a[i], MUL_BITS[i], 0x11110000u + (uint32_t)i, 0)) != 0 ||
            (rc = mpi_fixed(&g_mul_b[i], MUL_BITS[i], 0x22220000u + (uint32_t)i, 0)) != 0) {
            printf("[error] mul operand %u rc=-0x%04x\n", (unsigned)MUL_BITS[i], (unsigned)-rc);
            return rc;
        }
    }

    // e = 65537: the public exponent in every certificate chain this firmware
    // meets, so exp_mod here is an RSA *verify*, which is the operation #119
    // was about.
    if ((rc = mbedtls_mpi_lset(&g_exp_e, 65537)) != 0) {
        return rc;
    }
    mbedtls_mpi tmp;
    mbedtls_mpi_init(&tmp);
    rc = mpi_fixed(&g_exp2_n, 2048, 0x33330001u, 1);
    if (rc == 0) rc = mpi_fixed(&tmp, 2048, 0x44440001u, 0);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&g_exp2_a, &tmp, &g_exp2_n); // base < modulus
    if (rc == 0) rc = mpi_fixed(&g_exp4_n, 4096, 0x33330002u, 1);
    if (rc == 0) rc = mpi_fixed(&tmp, 4096, 0x44440002u, 0);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&g_exp4_a, &tmp, &g_exp4_n);
    if (rc == 0) rc = mpi_fixed(&tmp, 255, 0x55550001u, 1);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&g_inv_a, &tmp, &g_grp.P); // P is prime, so coprime
    mbedtls_mpi_free(&tmp);
    if (rc != 0) {
        printf("[error] exp/inv operand setup rc=-0x%04x\n", (unsigned)-rc);
        return rc;
    }

    return 0;
}

// ---- Reporting ----------------------------------------------------------
static void print_mpi(const char *label, const mbedtls_mpi *X)
{
    unsigned char buf[64];
    const size_t len = mbedtls_mpi_size(X);
    if (len > sizeof(buf) || mbedtls_mpi_write_binary(X, buf, len) != 0) {
        printf("[vec] %s=<unprintable>\n", label);
        return;
    }
    printf("[vec] %s=", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X", buf[i]);
    }
    printf("\n");
}

// Integer nanoseconds throughout, never a float: newlib-nano's printf silently
// drops %f, and a benchmark that prints its results wrong is worse than one
// that does not print them.
static int64_t per_op_ns(int64_t batch_us, int iters)
{
    return (batch_us * 1000) / iters;
}

static void run_bench(const bench_t *b)
{
    g_last_rc = 0;

    const int64_t c0 = esp_timer_get_time();
    b->fn();
    int64_t one_us = esp_timer_get_time() - c0;
    if (one_us < 1) {
        one_us = 1;
    }
    int iters = (int)(BATCH_TARGET_US / one_us);
    if (iters < 1) {
        iters = 1;
    }
    if (iters > MAX_ITERS) {
        iters = MAX_ITERS;
    }

    spike_counters_reset();

    int64_t best = INT64_MAX, worst = 0, total = 0;
    for (int rep = 0; rep < REPS; rep++) {
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < iters; i++) {
            b->fn();
        }
        const int64_t dt = esp_timer_get_time() - t0;
        total += dt;
        if (dt < best) best = dt;
        if (dt > worst) worst = dt;
        // Let the idle task on this core run, so the task watchdog stays quiet
        // and a long sweep cannot be mistaken for a hang.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    spike_counters_t c;
    spike_counters_read(&c);
    const int64_t ops = (int64_t)iters * REPS;

    // mul_calls_per_op and exp_calls_per_op are scaled by 1000, because they
    // are integers and a fractional call count is real: an ECDSA verify makes
    // thousands of multiplies, but a 4096-bit exp_mod makes none at all.
    printf("[bench] name=%s bits=%u iters=%d reps=%d "
           "min_ns=%lld mean_ns=%lld max_ns=%lld "
           "mul_calls_per_op=%lld mul_ns_per_op=%lld "
           "exp_calls_per_op=%lld exp_ns_per_op=%lld rc=%d\n",
           b->name, b->bits, iters, REPS,
           (long long)per_op_ns(best, iters),
           (long long)(total * 1000 / ops),
           (long long)per_op_ns(worst, iters),
           (long long)((int64_t)c.mul_calls * 1000 / ops),
           (long long)(c.mul_us * 1000 / ops),
           (long long)((int64_t)c.exp_calls * 1000 / ops),
           (long long)(c.exp_us * 1000 / ops),
           g_last_rc);

    if (g_last_rc != 0) {
        printf("[warn] %s/%u returned non-zero (-0x%04x) -- its timing is not "
               "a timing of the intended work\n",
               b->name, b->bits, (unsigned)-g_last_rc);
    }
}

static void bench_task(void *arg)
{
    (void)arg;

    printf("\n[id] mbedtls=%s idf=%s hw_mul=%d hw_exp=%d fixed_point=%d wrap=%d "
           "cpu_mhz=%d opt=%s core=%d free_internal=%u\n",
           MBEDTLS_VERSION_STRING, IDF_VER, SPIKE_HW_MUL, SPIKE_HW_EXP, SPIKE_FIXED_POINT,
           SPIKE_NO_WRAP ? 0 : 1,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           // IDF 6 has a per-component mbedTLS optimisation choice; IDF 5.5 does
           // not, and there mbedTLS inherits the global level. Test both so the
           // banner reports the level mbedTLS was really built at on either SDK
           // rather than a default nobody checked.
#if defined(CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_PERF) || \
    (!defined(CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE) && defined(CONFIG_COMPILER_OPTIMIZATION_PERF))
           "mbedtls-O2",
#else
           "mbedtls-Os",
#endif
           xPortGetCoreID(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (setup() != 0) {
        printf("[fail] setup failed -- no measurements are valid\n");
        vTaskDelete(NULL);
        return;
    }

    print_mpi("d", &g_d);
    print_mpi("sig_r", &g_r);
    print_mpi("sig_s", &g_s);
    print_mpi("sig384_r", &g_r384);
    print_mpi("sig384_s", &g_s384);

    for (size_t i = 0; i < sizeof(BENCHES) / sizeof(BENCHES[0]); i++) {
        run_bench(&BENCHES[i]);

        // The wrap check has to ride on a benchmark that provably calls the
        // hooked symbol from another translation unit. A --wrap that fails to
        // resolve produces zero counts, which is indistinguishable from "this
        // version never calls it" unless it is checked here.
        if (i == 0) {
            spike_counters_t c;
            spike_counters_read(&c);
            if (SPIKE_NO_WRAP) {
                printf("[ok] no-wrap control build: hooks are not linked, so "
                       "every call count and mul_ns_per_op below reads zero by "
                       "construction. Timings here carry no instrumentation.\n");
            } else if (c.mul_calls == 0) {
                printf("[fail] -Wl,--wrap=mbedtls_mpi_mul_mpi did not take: an "
                       "ECDSA verify recorded zero calls. Every call count below "
                       "is meaningless.\n");
            } else {
                printf("[ok] wrap active (%u mul calls during the verify bench)\n",
                       (unsigned)c.mul_calls);
            }
        }
    }

    printf("[done] free_internal=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Core 1, as the firmware's own consumer loop is (ADR 0001), and away from
    // the core WiFi would use if this app had any. 24 KB of stack because a
    // software 4096-bit exp_mod is not frugal.
    xTaskCreatePinnedToCore(bench_task, "bench", 24576, NULL, 5, NULL, 1);
}
