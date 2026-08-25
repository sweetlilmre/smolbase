/* SPIKE: is an ECDSA verify through PSA the same price as one through the
 * legacy API, in one binary?
 *
 * Why this exists. Instrumenting x509_crt.c and pk_wrap.c showed that
 * mbedtls_x509_crt_verify is 99.8% two psa_verify_hash calls -- there is no
 * chain-walk overhead to find. But the arduino-esp32 build (mbedTLS 3.6.6,
 * MBEDTLS_USE_PSA_CRYPTO NOT defined) verifies certificate signatures through
 * the legacy mbedtls_ecdsa_verify path instead, and the primitive benchmarks in
 * spike/mbedtls-perf/ measure that legacy path on both versions. So the two
 * numbers being compared are not the same operation, and the harness figures
 * are far below what the certificate phase actually costs.
 *
 * This measures both APIs on identical operands in the same binary, which is
 * the only comparison this project trusts. Fixed key and fixed digest, taken
 * from the same RFC 6979 vectors the harness uses so the two are relatable.
 *
 * Temporary. Revert with the rest of the spike instrumentation.
 */

/* Must precede every mbedTLS include: in 4.x this is what un-hides the
 * built-in (non-PSA) declarations. Own translation unit for exactly that
 * reason -- Http.cpp has already included esp_tls.h by the time it gets here. */
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

#include "psa/crypto.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

/* printf rather than ESP_LOGW so the file is byte-identical in both builds:
 * arduino-esp32 compiles ESP_LOG* out below its own core debug level, and the
 * whole point of this probe is that the two builds run the same source.
 * Both SDKs route printf to UART0. */
#define SPIKE_PSA_LOG(fmt, ...) printf("W spike: " fmt "\n", ##__VA_ARGS__)

/* SPIKE: count every cross-translation-unit call to mbedtls_mpi_mul_mpi,
 * using the linker's --wrap exactly as spike/mbedtls-perf/main/wrap.c does.
 * This is the one measurement timing noise cannot corrupt, and it is the only
 * way to see inside the arduino build, whose mbedTLS is precompiled.
 *
 * Counting ONLY -- no esp_timer calls in the hook. The harness learnt the hard
 * way that a hook which times as well as counts costs the same order as the
 * effect under test. Times come from the unwrapped builds already measured.
 *
 * Enabled by -Wl,--wrap=mbedtls_mpi_mul_mpi at link time. Without it the file
 * still compiles and the counters read zero, which the report makes visible.
 *
 * Caveat carried from the harness: --wrap redirects only references the linker
 * resolves, so calls made inside the defining translation unit are invisible.
 * ecp.c is a separate unit from bignum.c and esp_bignum.c, so the ECP path --
 * the one that matters here -- is counted. */
#ifdef SPIKE_WRAP_MUL
extern "C" {
int __real_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A,
                               const mbedtls_mpi *B);
unsigned spike_mul_calls;
int __wrap_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A,
                               const mbedtls_mpi *B)
{
    spike_mul_calls++;
    return __real_mbedtls_mpi_mul_mpi(X, A, B);
}
}
#define SPIKE_MUL_RESET() (spike_mul_calls = 0)
#define SPIKE_MUL_COUNT() (spike_mul_calls)
#else
#define SPIKE_MUL_RESET() ((void) 0)
#define SPIKE_MUL_COUNT() (0u)
#endif

namespace {

constexpr int ITERS = 1;

/* RFC 6979 A.2.5 / A.2.6 private keys and the matching fixed digests, the same
 * ones spike/mbedtls-perf/main/bench.c uses. */
const char D256_HEX[] =
    "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721";
const char D384_HEX[] =
    "6B9D3DAD2E1B8C1C05B19875B6659F4DE23C3B667BF297BA9AA47740787137D8"
    "96D5724E4C70A825F872C9EA60D2EDF5";

const unsigned char HASH32[32] = {
    0xaf, 0x2b, 0xdb, 0xe1, 0xaa, 0x9b, 0x6e, 0xc1, 0xe2, 0xad, 0xe1, 0xd6,
    0x94, 0xf4, 0x1f, 0xc7, 0x1a, 0x83, 0x1d, 0x02, 0x68, 0xe9, 0x89, 0x15,
    0x62, 0x11, 0x3d, 0x8a, 0x62, 0xad, 0xd1, 0xbf,
};

const unsigned char HASH48[48] = {
    0x9a, 0x9d, 0xd5, 0x1f, 0xe9, 0x50, 0x92, 0x2a, 0xed, 0x1e, 0x8a, 0x2b,
    0x3b, 0x0d, 0x6a, 0x4c, 0x37, 0x77, 0x59, 0x18, 0x2b, 0xc1, 0x2a, 0x0e,
    0x9d, 0x4c, 0x71, 0x2d, 0x6a, 0x1f, 0x53, 0x84, 0x2f, 0xd6, 0x8b, 0x77,
    0x1c, 0x39, 0x0e, 0x55, 0x74, 0x2b, 0x9c, 0x11, 0x63, 0x8e, 0x0a, 0x52,
};

/* Fixed-seed xorshift, as in the harness: mbedTLS wants an RNG for signing and
 * for the blinded point multiply, and a real entropy source would make two runs
 * do different work on the same inputs. */
struct Prng {
    uint32_t s;
};

int prngFill(void *ctx, unsigned char *out, size_t len)
{
    Prng *p = static_cast<Prng *>(ctx);
    for (size_t i = 0; i < len; i++) {
        p->s ^= p->s << 13;
        p->s ^= p->s >> 17;
        p->s ^= p->s << 5;
        out[i] = static_cast<unsigned char>(p->s & 0xff);
    }
    return 0;
}

int64_t nowUs() { return esp_timer_get_time(); }

/* One curve's worth of the comparison. */
void benchCurve(const char *label, mbedtls_ecp_group_id gid,
                const char *d_hex, const unsigned char *hash, size_t hash_len,
                size_t coord_len)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d, r, s;
    Prng prng = {0xC0FFEEu};

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int rc = mbedtls_ecp_group_load(&grp, gid);
    /* grp.modp is the fast pseudo-reduction for this curve. NULL means the
     * group falls back to a generic modular reduction -- a division, and far
     * dearer than the multiply it follows. Non-multiply ECP work came out
     * cheaper for P-384 than for P-256 on one build, which no model allows, so
     * check the cheapest possible explanation first. */
    SPIKE_PSA_LOG("[spike-modp] %s modp=%s nbits=%u", label,
                  grp.modp ? "FAST" : "NULL(generic)", (unsigned) grp.nbits);
    rc |= mbedtls_mpi_read_string(&d, 16, d_hex);
    rc |= mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, prngFill, &prng);
    rc |= mbedtls_ecdsa_sign(&grp, &r, &s, &d, hash, hash_len, prngFill, &prng);
    if (rc != 0) {
        SPIKE_PSA_LOG("[spike-psa] %s setup failed rc=%d", label, rc);
        goto done;
    }

    {
        /* ---- legacy: what mbedTLS 3.6.6 without PSA does, and what
         * spike/mbedtls-perf/ measures on both versions ---- */
        SPIKE_MUL_RESET();
        int64_t t0 = nowUs();
        int vrc = 0;
        for (int i = 0; i < ITERS; i++) {
            vrc |= mbedtls_ecdsa_verify(&grp, hash, hash_len, &Q, &r, &s);
        }
        int64_t t1 = nowUs();
        const unsigned legacy_muls = SPIKE_MUL_COUNT() / (unsigned) ITERS;

        vTaskDelay(pdMS_TO_TICKS(20));

        /* ---- PSA: what mbedTLS 4's x509_crt_check_signature does, import and
         * destroy included, because that is the whole call it makes ---- */
        unsigned char pub[133];
        size_t pub_len = 0;
        unsigned char sig[132];
        int prc = mbedtls_ecp_point_write_binary(&grp, &Q,
                                                 MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                 &pub_len, pub, sizeof(pub));
        prc |= mbedtls_mpi_write_binary(&r, sig, coord_len);
        prc |= mbedtls_mpi_write_binary(&s, sig + coord_len, coord_len);
        if (prc != 0) {
            SPIKE_PSA_LOG("[spike-psa] %s export failed rc=%d", label, prc);
            goto done;
        }

        psa_crypto_init();
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&attr, PSA_ALG_ECDSA_ANY);

        psa_status_t st = PSA_SUCCESS;
        SPIKE_MUL_RESET();
        int64_t t2 = nowUs();
        for (int i = 0; i < ITERS; i++) {
            mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
            psa_status_t a = psa_import_key(&attr, pub, pub_len, &kid);
            psa_status_t b = psa_verify_hash(kid, PSA_ALG_ECDSA_ANY, hash,
                                             hash_len, sig, 2 * coord_len);
            psa_destroy_key(kid);
            if (a != PSA_SUCCESS) { st = a; }
            if (b != PSA_SUCCESS) { st = b; }
        }
        int64_t t3 = nowUs();
        const unsigned psa_muls = SPIKE_MUL_COUNT() / (unsigned) ITERS;

        const unsigned long legacy_us = (unsigned long)((t1 - t0) / ITERS);
        const unsigned long psa_us = (unsigned long)((t3 - t2) / ITERS);
        SPIKE_PSA_LOG("[spike-psa] %s legacy=%lu us psa=%lu us ratio=%lu.%02lu "
                      "muls=%u/%u (legacy rc=%d psa st=%d)",
                      label, legacy_us, psa_us,
                      legacy_us ? psa_us / legacy_us : 0UL,
                      legacy_us ? (psa_us * 100UL / legacy_us) % 100UL : 0UL,
                      legacy_muls, psa_muls, vrc, (int)st);
    }

done:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
}

/* Both libraries make exactly the same number of mbedtls_mpi_mul_mpi calls per
 * verify -- 5817 for P-256, 8569 for P-384 -- so the version difference is
 * price per call, not number of calls. This times the call itself at both
 * operand sizes, in the firmware, so the two builds can be compared on the one
 * quantity the call count leaves open. */
void benchMulSize(const char *label, size_t bits, int iters)
{
    mbedtls_mpi a, b, x;
    unsigned char buf[64];
    const size_t len = bits / 8;
    Prng p = {0xBEEFu};

    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&x);

    prngFill(&p, buf, len);
    buf[0] |= 0x80;                 /* exactly `bits` wide, so the size is honest */
    int rc = mbedtls_mpi_read_binary(&a, buf, len);
    prngFill(&p, buf, len);
    buf[0] |= 0x80;
    rc |= mbedtls_mpi_read_binary(&b, buf, len);
    if (rc != 0) {
        SPIKE_PSA_LOG("[spike-mul] %s setup failed rc=%d", label, rc);
    } else {
        int64_t t0 = nowUs();
        for (int i = 0; i < iters; i++) {
            rc |= mbedtls_mpi_mul_mpi(&x, &a, &b);
        }
        int64_t t1 = nowUs();
        const unsigned long ns = (unsigned long)(((t1 - t0) * 1000) / iters);
        SPIKE_PSA_LOG("[spike-mul] %s %u bits: %lu.%03lu us/call over %d calls rc=%d",
                      label, (unsigned) bits, ns / 1000, ns % 1000, iters, rc);
    }

    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&a);
}

/* The same two curves again, but on a task pinned to core 1 the way
 * spike/mbedtls-perf/ pins its benchmark task. The harness reports 234 us*1000
 * for a P-256 verify in this exact lever configuration and the firmware reads
 * ~420 ms for the same call, so either the harness numbers do not transfer to a
 * running firmware or the difference is scheduling. This is the discriminator:
 * same binary, same operands, only the execution context changes. */
struct PinnedArgs {
    TaskHandle_t waiter;
    const char *label256;
    const char *label384;   /* nullptr skips the P-384 leg */
};

void pinnedTask(void *arg)
{
    PinnedArgs *a = static_cast<PinnedArgs *>(arg);
    if (a->label384 != nullptr) {
        /* Fresh MPIs, no ECP state, one operand size at a time. */
        benchMulSize(a->label256, 256, 4000);
        benchMulSize(a->label256, 384, 4000);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    benchCurve(a->label256, MBEDTLS_ECP_DP_SECP256R1, D256_HEX, HASH32,
               sizeof(HASH32), 32);
    if (a->label384 != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
        benchCurve(a->label384, MBEDTLS_ECP_DP_SECP384R1, D384_HEX, HASH48,
                   sizeof(HASH48), 48);
    }
    xTaskNotifyGive(a->waiter);
    vTaskDelete(nullptr);
}

void runPinned(int core, const char *l256, const char *l384, int prio = 5)
{
    PinnedArgs args = {xTaskGetCurrentTaskHandle(), l256, l384};
    if (xTaskCreatePinnedToCore(pinnedTask, "spikepin", 16384, &args, prio,
                                nullptr, core) != pdPASS) {
        SPIKE_PSA_LOG("[spike-psa] pinned task create failed (core %d)", core);
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

} // namespace

extern "C" void spike_psa_vs_legacy(void)
{
    benchCurve("p256-inline", MBEDTLS_ECP_DP_SECP256R1, D256_HEX, HASH32,
               sizeof(HASH32), 32);
    vTaskDelay(pdMS_TO_TICKS(20));
    benchCurve("p384-inline", MBEDTLS_ECP_DP_SECP384R1, D384_HEX, HASH48,
               sizeof(HASH48), 48);

    runPinned(1, "p256-core1", "p384-core1");
    runPinned(0, "p256-core0", "p384-core0");
    /* Preemption or cache? At priority 24 the bench outranks the WiFi task
     * (23) and the lwIP task (18) on their own core. If the cost collapses to
     * the core-1 figure it was scheduling; if it stays, it is contention the
     * scheduler cannot arbitrate. P-256 only: starving WiFi for the length of
     * a P-384 verify is not worth the answer. */
    runPinned(0, "p256-core0-prio24", nullptr, 24);
}
