/* SPIKE: X.509 parse and chain-verify benchmark, written so the SAME source
 * compiles in both the ESP-IDF build and the arduino-esp32 v0.3.3 build.
 *
 * It exists because the certificate phase is 58% of the handshake and roughly
 * 1250 ms of it is neither the leaf verify nor the root verify -- it is X.509
 * parsing and chain walking, which nothing in this spike has ever measured.
 * The arduino build's mbedTLS is precompiled, so this has to live in
 * application code and call the public API rather than instrument the library.
 *
 * Same DER in both builds (spike_chain.h), same iteration counts, same clock
 * source semantics (milliseconds).
 */
#include "spike_chain.h"

#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <string.h>

#ifndef SPIKE_LOG
#error "define SPIKE_LOG(fmt, ...) before including"
#endif
#ifndef SPIKE_MS
#error "define SPIKE_MS() returning milliseconds before including"
#endif

/* Counters exported by the temporarily instrumented library-side
 * x509_crt.c. Only our ESP-IDF build can define this: the arduino build's
 * mbedTLS is precompiled, so the same source compiles there with the dump
 * reduced to nothing. */
#ifdef SPIKE_X509_LIB_PROF
extern "C" {
extern unsigned long long spike_x509_cyc_hash, spike_x509_cyc_verify;
extern unsigned long long spike_x509_cyc_cando, spike_x509_cyc_parent;
extern unsigned long long spike_x509_cyc_namecmp, spike_x509_cyc_chain;
extern unsigned spike_x509_n_hash, spike_x509_n_verify;
extern unsigned spike_x509_n_cando, spike_x509_n_parent, spike_x509_n_namecmp;
extern unsigned long long spike_pk_cyc_import, spike_pk_cyc_verify;
extern unsigned long long spike_pk_cyc_destroy, spike_pk_cyc_der2raw;
extern unsigned spike_pk_n_verify;
}
/* 240 MHz core clock: cycles / 240 = microseconds. */
#define SPIKE_US(c) ((unsigned long)((c) / 240ULL))
/* The counters are free-running from boot, so a single bench has to zero
 * them or it reads every handshake since. */
#define SPIKE_X509_RESET_LIB_PROF()                                           \
    do {                                                                      \
        spike_x509_cyc_hash = spike_x509_cyc_verify = 0;                      \
        spike_x509_cyc_cando = spike_x509_cyc_parent = 0;                     \
        spike_x509_cyc_namecmp = spike_x509_cyc_chain = 0;                    \
        spike_x509_n_hash = spike_x509_n_verify = 0;                          \
        spike_x509_n_cando = spike_x509_n_parent = spike_x509_n_namecmp = 0;  \
        spike_pk_cyc_import = spike_pk_cyc_verify = 0;                        \
        spike_pk_cyc_destroy = spike_pk_cyc_der2raw = 0;                      \
        spike_pk_n_verify = 0;                                                \
    } while (0)
#define SPIKE_X509_DUMP_LIB_PROF()                                            \
    do {                                                                      \
        SPIKE_LOG("[spike-x509] pk: import=%lu us | psa_verify=%lu us | "     \
                  "der2raw=%lu us | destroy=%lu us | n=%u",                   \
                  SPIKE_US(spike_pk_cyc_import),                              \
                  SPIKE_US(spike_pk_cyc_verify),                              \
                  SPIKE_US(spike_pk_cyc_der2raw),                             \
                  SPIKE_US(spike_pk_cyc_destroy), spike_pk_n_verify);         \
        SPIKE_LOG("[spike-x509] lib: chain=%lu us | verify_ext=%lu us x%u | " \
                  "hash=%lu us x%u | can_do=%lu us x%u | "                    \
                  "check_parent=%lu us x%u (name_cmp=%lu us x%u)",            \
                  SPIKE_US(spike_x509_cyc_chain),                             \
                  SPIKE_US(spike_x509_cyc_verify), spike_x509_n_verify,       \
                  SPIKE_US(spike_x509_cyc_hash), spike_x509_n_hash,           \
                  SPIKE_US(spike_x509_cyc_cando), spike_x509_n_cando,         \
                  SPIKE_US(spike_x509_cyc_parent), spike_x509_n_parent,       \
                  SPIKE_US(spike_x509_cyc_namecmp), spike_x509_n_namecmp);    \
    } while (0)
#else
#define SPIKE_X509_DUMP_LIB_PROF() do { } while (0)
#define SPIKE_X509_RESET_LIB_PROF() do { } while (0)
#endif

#define SPIKE_PARSE_ITERS  20
/* One iteration only: the arduino-esp32 build ships
 * CONFIG_ESP_TASK_WDT_PANIC=y, and five chain verifications in a row
 * overran the 5 s task watchdog and panicked the device -- which is the
 * very symptom this whole investigation started from. */
#define SPIKE_VERIFY_ITERS 1

static void spike_x509_bench(void)
{
    /* ---- parse: all three certs, from DER, into a fresh chain each time ---- */
    unsigned long t0 = SPIKE_MS();
    int rc = 0;
    for (int i = 0; i < SPIKE_PARSE_ITERS; i++) {
        mbedtls_x509_crt c;
        mbedtls_x509_crt_init(&c);
        rc |= mbedtls_x509_crt_parse_der(&c, SPIKE_CERT0, sizeof(SPIKE_CERT0));
        rc |= mbedtls_x509_crt_parse_der(&c, SPIKE_CERT1, sizeof(SPIKE_CERT1));
        rc |= mbedtls_x509_crt_parse_der(&c, SPIKE_CERT2, sizeof(SPIKE_CERT2));
        mbedtls_x509_crt_free(&c);
    }
    unsigned long t1 = SPIKE_MS();
    SPIKE_LOG("[spike-x509] parse_3certs x%d = %lu ms total, %lu ms each, rc=%d",
              SPIKE_PARSE_ITERS, (unsigned long)(t1 - t0),
              (unsigned long)((t1 - t0) / SPIKE_PARSE_ITERS), rc);

    /* ---- verify: leaf+intermediate against the top cert as trust anchor ----
     * Not the real trust decision, but exactly the same signature checks and
     * chain walk the handshake performs, and identical in both builds. */
    mbedtls_x509_crt chain, trust;
    mbedtls_x509_crt_init(&chain);
    mbedtls_x509_crt_init(&trust);
    rc = 0;
    rc |= mbedtls_x509_crt_parse_der(&chain, SPIKE_CERT0, sizeof(SPIKE_CERT0));
    rc |= mbedtls_x509_crt_parse_der(&chain, SPIKE_CERT1, sizeof(SPIKE_CERT1));
    rc |= mbedtls_x509_crt_parse_der(&trust, SPIKE_CERT2, sizeof(SPIKE_CERT2));
    if (rc != 0) {
        SPIKE_LOG("[spike-x509] setup parse failed rc=%d", rc);
    } else {
        uint32_t flags = 0;
        int vrc = 0;
        SPIKE_X509_RESET_LIB_PROF();
        t0 = SPIKE_MS();
        for (int i = 0; i < SPIKE_VERIFY_ITERS; i++) {
            flags = 0;
            vrc = mbedtls_x509_crt_verify(&chain, &trust, NULL, NULL, &flags,
                                          NULL, NULL);
        }
        t1 = SPIKE_MS();
        SPIKE_LOG("[spike-x509] crt_verify x%d = %lu ms total, %lu ms each, rc=%d flags=0x%x",
                  SPIKE_VERIFY_ITERS, (unsigned long)(t1 - t0),
                  (unsigned long)((t1 - t0) / SPIKE_VERIFY_ITERS), vrc,
                  (unsigned)flags);
        SPIKE_X509_DUMP_LIB_PROF();
    }

    mbedtls_x509_crt_free(&chain);
    mbedtls_x509_crt_free(&trust);
}
