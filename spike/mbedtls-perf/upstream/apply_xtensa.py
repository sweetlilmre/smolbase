#!/usr/bin/env python3
r"""Add an Xtensa assembly path to mbedTLS's constant-time primitives.

Rationale (established by MemSan constant-flow testing on x86-64, with a
working negative control): every C-level reformulation of these primitives
that removes mbedtls_ct_compiler_opaque() barriers is detected as
non-constant-flow, because the optimiser recognises the branchless idiom and
re-derives a comparison. Barriers on the inputs alone are not enough -- they
hide values, not structure. Assembly is opaque by construction, which is
exactly why the Arm/AArch64/x86 paths exist. Xtensa simply never got one.

Usage: apply_xtensa.py <path-to-constant_time_impl.h>
"""
import io
import sys

path = sys.argv[1]
s = io.open(path, encoding="utf-8").read()


def sub1(text, old, new, what):
    if text.count(old) != 1:
        sys.exit("PATCH ERROR: %s matched %d times" % (what, text.count(old)))
    return text.replace(old, new)


# 1. Gate: define MBEDTLS_CT_XTENSA_ASM alongside the existing architectures.
s = sub1(
    s,
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

# 2. mbedtls_ct_bool: all bits set iff x != 0.  neg / or / srai, mirroring the
#    AArch64 sequence.
s = sub1(
    s,
    """#elif defined(MBEDTLS_CT_X86_ASM) && defined(MBEDTLS_CT_SIZE_32)
    uint32_t s;
    asm volatile ("mov %[x], %[s]                                 \\n\\t"
                  "neg %[s]                                       \\n\\t"
                  "or %[s], %[x]                                  \\n\\t"
                  "sar $31, %[x]                                  \\n\\t"
                  :
                  [s] "=&c" (s),
                  [x] "+&a" (x)
                  :
                  :
                  );
    return (mbedtls_ct_condition_t) x;
#else""",
    """#elif defined(MBEDTLS_CT_X86_ASM) && defined(MBEDTLS_CT_SIZE_32)
    uint32_t s;
    asm volatile ("mov %[x], %[s]                                 \\n\\t"
                  "neg %[s]                                       \\n\\t"
                  "or %[s], %[x]                                  \\n\\t"
                  "sar $31, %[x]                                  \\n\\t"
                  :
                  [s] "=&c" (s),
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
#else""",
    "ct_bool xtensa",
)

# 3. mbedtls_ct_if: (condition & if1) | (~condition & if0).
s = sub1(
    s,
    """#elif defined(MBEDTLS_CT_X86_ASM) && defined(MBEDTLS_CT_SIZE_32)
    asm volatile ("and %[condition], %[if1]                       \\n\\t"
                  "not %[condition]                               \\n\\t"
                  "and %[if0], %[condition]                       \\n\\t"
                  "or %[condition], %[if1]                        \\n\\t"
                  :
                  [condition] "+&c" (condition),
                  [if1] "+&a" (if1)
                  :
                  [if0] "b" (if0)
                  :
                  );
    return if1;
#else""",
    """#elif defined(MBEDTLS_CT_X86_ASM) && defined(MBEDTLS_CT_SIZE_32)
    asm volatile ("and %[condition], %[if1]                       \\n\\t"
                  "not %[condition]                               \\n\\t"
                  "and %[if0], %[condition]                       \\n\\t"
                  "or %[condition], %[if1]                        \\n\\t"
                  :
                  [condition] "+&c" (condition),
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

# 4. mbedtls_ct_uint_lt: borrow out of x - y, broadcast.  Same six-operation
#    sequence as the Arm path (eor/sub/bic/and/orr/asr), plus the movi/xor
#    that stands in for Xtensa's missing bic.
s = sub1(
    s,
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

io.open(path, "w", encoding="utf-8", newline="").write(s)
print("Xtensa asm paths added to " + path)
