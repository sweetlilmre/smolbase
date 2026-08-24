#!/usr/bin/env python3
r"""Generate the complete upstream fix in a pristine TF-PSA-Crypto tree.

Three parts:
  1. Xtensa asm for mbedtls_ct_bool / mbedtls_ct_if / mbedtls_ct_uint_lt.
  2. MBEDTLS_CT_INLINE - force inlining, but ONLY where an asm path is in use.
  3. _if_else_0 at the two bignum_core call sites.

Usage: apply_fix.py <tf-psa-crypto-root>
"""
import io
import sys

root = sys.argv[1]
CT = root + "/utilities/constant_time_impl.h"
BN = root + "/drivers/builtin/src/bignum_core.c"


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def sub1(text, old, new, what):
    if text.count(old) != 1:
        sys.exit("PATCH ERROR: %s matched %d times" % (what, text.count(old)))
    return text.replace(old, new)


ct = rd(CT)

# ---- 1. architecture gate -------------------------------------------------
ct = sub1(
    ct,
    """#elif defined(__i386__)
#define MBEDTLS_CT_X86_ASM
#endif
#endif
""",
    """#elif defined(__i386__)
#define MBEDTLS_CT_X86_ASM
#elif defined(__XTENSA__)
#define MBEDTLS_CT_XTENSA_ASM
#endif
#endif

/* Force inlining of the constant-time primitives.
 *
 * At -Os GCC leaves these out of line even though the asm bodies are a
 * handful of instructions, so every use pays a call. This is not specific to
 * one target: measured on mbedtls_mpi_core_sub(), which calls
 * mbedtls_ct_uint_lt() twice per limb,
 *
 *   arm-none-eabi-gcc -mthumb -mcpu=cortex-m4 -Os : 2 calls, not inlined
 *   arm-none-eabi-gcc -mthumb -mcpu=cortex-m4 -O2 : 0 calls, inlined
 *   xtensa-esp-elf-gcc                        -Os : 2 calls, not inlined
 *   x86-64 gcc/clang                      -Os/-O2 : 0 calls, always inlined
 *
 * So existing Arm -Os builds pay this too; x86-64 is the outlier that inlines
 * regardless, which is presumably why it went unnoticed. On Xtensa a call is
 * especially expensive because of the register-window entry and spills.
 *
 * Cost on Arm thumb -Os, whole translation unit: +8 bytes of .text.
 *
 * Deliberately NOT applied to the generic C fallback, which is much larger
 * (several mbedtls_ct_compiler_opaque() barriers): there the same change costs
 * +396 bytes of .text on the same measurement, which is a trade-off for the
 * maintainers to make rather than a clear win. Reaching the first branch
 * implies __GNUC__, because MBEDTLS_CT_ASM above requires it.
 */
#if defined(MBEDTLS_CT_ARM_ASM) || defined(MBEDTLS_CT_AARCH64_ASM) || \\
    defined(MBEDTLS_CT_X86_64_ASM) || defined(MBEDTLS_CT_X86_ASM) || \\
    defined(MBEDTLS_CT_XTENSA_ASM)
#define MBEDTLS_CT_INLINE static inline __attribute__((always_inline))
#else
#define MBEDTLS_CT_INLINE static inline
#endif
""",
    "arch gate + MBEDTLS_CT_INLINE",
)

# ---- 2. mbedtls_ct_bool: neg / or / srai ----------------------------------
ct = sub1(
    ct,
    """                  [s] "=&c" (s),
                  [x] "+&a" (x)
                  :
                  :
                  );
    return (mbedtls_ct_condition_t) x;
#else
    const mbedtls_ct_uint_t xo = mbedtls_ct_compiler_opaque(x);""",
    """                  [s] "=&c" (s),
                  [x] "+&a" (x)
                  :
                  :
                  );
    return (mbedtls_ct_condition_t) x;
#elif defined(MBEDTLS_CT_XTENSA_ASM) && defined(MBEDTLS_CT_SIZE_32)
    uint32_t s;
    asm volatile ("neg   %[s], %[x]                               \\n\\t"
                  "or    %[x], %[s], %[x]                         \\n\\t"
                  "srai  %[x], %[x], 31                           \\n\\t"
                  :
                  [s] "=&a" (s),
                  [x] "+&a" (x)
                  :
                  :
                  );
    return (mbedtls_ct_condition_t) x;
#else
    const mbedtls_ct_uint_t xo = mbedtls_ct_compiler_opaque(x);""",
    "ct_bool xtensa",
)

# ---- 3. mbedtls_ct_if: (cond & if1) | (~cond & if0) -----------------------
ct = sub1(
    ct,
    """                  [condition] "+&c" (condition),
                  [if1] "+&a" (if1)
                  :
                  [if0] "b" (if0)
                  :
                  );
    return if1;
#else""",
    """                  [condition] "+&c" (condition),
                  [if1] "+&a" (if1)
                  :
                  [if0] "b" (if0)
                  :
                  );
    return if1;
#elif defined(MBEDTLS_CT_XTENSA_ASM) && defined(MBEDTLS_CT_SIZE_32)
    /* Xtensa has no and-not, so ~condition is built with movi -1 / xor. */
    uint32_t n, t;
    asm volatile ("movi  %[n], -1                                 \\n\\t"
                  "xor   %[n], %[condition], %[n]                 \\n\\t"
                  "and   %[t], %[condition], %[if1]               \\n\\t"
                  "and   %[n], %[n], %[if0]                       \\n\\t"
                  "or    %[t], %[t], %[n]                         \\n\\t"
                  :
                  [n] "=&a" (n),
                  [t] "=&a" (t)
                  :
                  [condition] "a" (condition),
                  [if1] "a" (if1),
                  [if0] "a" (if0)
                  :
                  );
    return (mbedtls_ct_uint_t) t;
#else""",
    "ct_if xtensa",
)

# ---- 4. mbedtls_ct_uint_lt: borrow out of x - y, broadcast ---------------
ct = sub1(
    ct,
    """    return (mbedtls_ct_condition_t) x;
#else
    /* Ensure that the compiler cannot optimise the following operations over x and y,""",
    """    return (mbedtls_ct_condition_t) x;
#elif defined(MBEDTLS_CT_XTENSA_ASM) && defined(MBEDTLS_CT_SIZE_32)
    uint32_t s, n, t;
    asm volatile ("xor   %[s], %[x], %[y]                         \\n\\t"
                  "sub   %[t], %[x], %[y]                         \\n\\t"
                  "movi  %[n], -1                                 \\n\\t"
                  "xor   %[n], %[s], %[n]                         \\n\\t"
                  "and   %[t], %[t], %[n]                         \\n\\t"
                  "and   %[s], %[s], %[y]                         \\n\\t"
                  "or    %[t], %[t], %[s]                         \\n\\t"
                  "srai  %[t], %[t], 31                           \\n\\t"
                  :
                  [s] "=&a" (s),
                  [n] "=&a" (n),
                  [t] "=&a" (t)
                  :
                  [x] "a" (x),
                  [y] "a" (y)
                  :
                  );
    return (mbedtls_ct_condition_t) t;
#else
    /* Ensure that the compiler cannot optimise the following operations over x and y,""",
    "ct_uint_lt xtensa",
)

# ---- 5. apply MBEDTLS_CT_INLINE to the three definitions -----------------
for sig, what in [
    ("static inline mbedtls_ct_condition_t mbedtls_ct_bool(mbedtls_ct_uint_t x)\n{",
     "ct_bool inline"),
    ("static inline mbedtls_ct_uint_t mbedtls_ct_if(mbedtls_ct_condition_t condition,",
     "ct_if inline"),
    ("static inline mbedtls_ct_condition_t mbedtls_ct_uint_lt(mbedtls_ct_uint_t x, "
     "mbedtls_ct_uint_t y)\n{", "ct_uint_lt inline"),
]:
    ct = sub1(ct, sig, sig.replace("static inline", "MBEDTLS_CT_INLINE", 1), what)

wr(CT, ct)

# ---- 6. call sites -------------------------------------------------------
bn = rd(BN)
bn = sub1(
    bn,
    """        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(A[i], c),
                                                    1, 0);
        mbedtls_mpi_uint t = A[i] - c;
        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(t, B[i]), 1, 0) + z;""",
    """        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if_else_0(
            mbedtls_ct_uint_lt(A[i], c), 1);
        mbedtls_mpi_uint t = A[i] - c;
        c = mbedtls_ct_mpi_uint_if_else_0(mbedtls_ct_uint_lt(t, B[i]), 1) + z;""",
    "core_sub call sites",
)
bn = sub1(
    bn,
    "        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(*d, c), 1, 0);",
    "        c = mbedtls_ct_mpi_uint_if_else_0(mbedtls_ct_uint_lt(*d, c), 1);",
    "core_mla call site",
)
wr(BN, bn)

print("complete fix applied under " + root)
