#!/usr/bin/env python3
r"""Apply one stage of the three-patch series to a TF-PSA-Crypto checkout.

  1  MBEDTLS_CT_INLINE       - force inlining on the EXISTING asm paths.
                               Independent; benefits Arm -Os today.
  2  Xtensa asm path         - new architecture. Independent of 1, except it
                               adds Xtensa to stage 1's macro list if present.
  3  bignum_core call sites  - _if_else_0. Independent of both.

Usage: gen_patches.py <stage> <tf-psa-crypto-root>
"""
import io
import sys

stage, root = sys.argv[1], sys.argv[2]
CT = root + "/utilities/constant_time_impl.h"
BN = root + "/drivers/builtin/src/bignum_core.c"


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def sub1(t, o, n, what):
    if t.count(o) != 1:
        sys.exit("PATCH ERROR: %s matched %d times" % (what, t.count(o)))
    return t.replace(o, n)


THREE_SIGS = [
    ("static inline mbedtls_ct_condition_t mbedtls_ct_bool(mbedtls_ct_uint_t x)\n{",
     "ct_bool"),
    ("static inline mbedtls_ct_uint_t mbedtls_ct_if(mbedtls_ct_condition_t condition,",
     "ct_if"),
    ("static inline mbedtls_ct_condition_t mbedtls_ct_uint_lt(mbedtls_ct_uint_t x, "
     "mbedtls_ct_uint_t y)\n{", "ct_uint_lt"),
]

# --------------------------------------------------------------------------
if stage == "1":
    ct = rd(CT)
    ct = sub1(
        ct,
        """#define MBEDTLS_CT_SIZE (sizeof(mbedtls_ct_uint_t) * 8)""",
        """/* Force inlining of the constant-time primitives where an assembly path is
 * in use.
 *
 * At -Os the compiler leaves these out of line even though the asm bodies are
 * a handful of instructions, so every use pays a call. Measured on
 * mbedtls_mpi_core_sub(), which calls mbedtls_ct_uint_lt() twice per limb:
 *
 *   arm-none-eabi-gcc -mthumb -mcpu=cortex-m4 -Os : 2 calls, not inlined
 *   arm-none-eabi-gcc -mthumb -mcpu=cortex-m4 -O2 : 0 calls, inlined
 *   arm-none-eabi-gcc -marm   -mcpu=arm7tdmi  -Os : 2 calls, not inlined
 *   x86-64 gcc and clang                  -Os/-O2 : 0 calls, always inlined
 *
 * x86-64 inlines regardless, which is presumably why this has gone unnoticed.
 * Cost on the Arm thumb -Os measurement: +8 bytes of .text for the whole
 * translation unit.
 *
 * Deliberately NOT applied to the generic C fallback, which is much larger:
 * there the same change costs +396 bytes on the same measurement, which is a
 * size/speed trade-off rather than a clear win. Reaching the first branch
 * implies __GNUC__, because MBEDTLS_CT_ASM above requires it.
 */
#if defined(MBEDTLS_CT_ARM_ASM) || defined(MBEDTLS_CT_AARCH64_ASM) || \\
    defined(MBEDTLS_CT_X86_64_ASM) || defined(MBEDTLS_CT_X86_ASM)
#define MBEDTLS_CT_INLINE static inline __attribute__((always_inline))
#else
#define MBEDTLS_CT_INLINE static inline
#endif

#define MBEDTLS_CT_SIZE (sizeof(mbedtls_ct_uint_t) * 8)""",
        "MBEDTLS_CT_INLINE macro",
    )
    for sig, what in THREE_SIGS:
        ct = sub1(ct, sig, sig.replace("static inline", "MBEDTLS_CT_INLINE", 1), what)
    wr(CT, ct)

# --------------------------------------------------------------------------
elif stage == "2":
    ct = rd(CT)
    ct = sub1(
        ct,
        """#elif defined(__i386__)
#define MBEDTLS_CT_X86_ASM
#endif""",
        """#elif defined(__i386__)
#define MBEDTLS_CT_X86_ASM
#elif defined(__XTENSA__)
#define MBEDTLS_CT_XTENSA_ASM
#endif""",
        "arch gate",
    )
    # If patch 1 is present, Xtensa joins its list. Optional so this patch
    # applies on its own too.
    old_list = """#if defined(MBEDTLS_CT_ARM_ASM) || defined(MBEDTLS_CT_AARCH64_ASM) || \\
    defined(MBEDTLS_CT_X86_64_ASM) || defined(MBEDTLS_CT_X86_ASM)"""
    if old_list in ct:
        ct = sub1(ct, old_list, old_list + """ || \\
    defined(MBEDTLS_CT_XTENSA_ASM)""", "add Xtensa to inline list")

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
    wr(CT, ct)

# --------------------------------------------------------------------------
elif stage == "3":
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

else:
    sys.exit("unknown stage " + stage)

print("stage %s applied" % stage)
