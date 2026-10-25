/* Targeted constant-flow harness for mbedtls_mpi_core_sub.
 *
 * Scoped to core_sub alone. The earlier version also exercised core_mla, but
 * passing the multiplier S[0] by value while poisoned made MSan report in the
 * harness's own frame rather than inside the library, which tells us nothing.
 *
 * Why not the upstream test suite: test_suite_bignum_core's cases compare the
 * returned carry (TEST_EQUAL at test_suite_bignum_core.function:825) while the
 * inputs are still marked secret. The carry is secret-derived by construction,
 * so under MemSan that reports for ANY implementation -- verified: stock
 * upstream and the plain-C revert produce byte-identical reports there. On x86
 * builds with MBEDTLS_HAVE_ASM the inline asm launders the poison and the
 * tests pass, which is why upstream CI never sees it -- and also means the
 * generic C path is not covered by upstream constant-flow testing at all.
 *
 * Here inputs are poisoned with __msan_allocated_memory (exactly what
 * TEST_CF_SECRET expands to) and every output is unpoisoned before it is
 * touched. Any report is therefore a branch or memory access INSIDE
 * mbedtls_mpi_core_sub that depends on secret data.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sanitizer/msan_interface.h>

typedef uint64_t mbedtls_mpi_uint;

extern mbedtls_mpi_uint mbedtls_mpi_core_sub(mbedtls_mpi_uint *X,
                                             const mbedtls_mpi_uint *A,
                                             const mbedtls_mpi_uint *B,
                                             size_t limbs);

#define N 8

static mbedtls_mpi_uint A[N], B[N], X[N];

int main(void)
{
    for (unsigned seed = 0; seed < 256; seed++) {
        /* Mixed magnitudes so borrows occur on some limbs and not others: a
         * data-dependent branch must actually be taken sometimes. */
        for (int i = 0; i < N; i++) {
            A[i] = 0x0123456789abcdefULL * (seed + 1) ^ (mbedtls_mpi_uint) i;
            B[i] = 0xfedcba9876543210ULL * (seed + 3) ^ (mbedtls_mpi_uint) (i * 7);
        }

        __msan_allocated_memory(A, sizeof(A));   /* == TEST_CF_SECRET */
        __msan_allocated_memory(B, sizeof(B));

        mbedtls_mpi_uint c = mbedtls_mpi_core_sub(X, A, B, N);

        __msan_unpoison(&c, sizeof(c));          /* == TEST_CF_PUBLIC */
        __msan_unpoison(X, sizeof(X));
        __msan_unpoison(A, sizeof(A));
        __msan_unpoison(B, sizeof(B));
    }

    printf("CONSTANT-FLOW OK: no secret-dependent branch or memory access "
           "inside mbedtls_mpi_core_sub\n");
    return 0;
}
