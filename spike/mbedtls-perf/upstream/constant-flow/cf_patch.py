#!/usr/bin/env python3
"""Apply one of the constant-flow spike variants to a TF-PSA-Crypto checkout.

Variants:
  stock   - leave upstream code untouched (baseline)
  revert  - restore mbedTLS 3.6.6's plain-C carry/borrow (NEGATIVE CONTROL:
            this is variable-time and the suite MUST flag it, otherwise the
            suite proves nothing about the fix)
  fix     - the proposed fix: branchless barrier-free generic ct_uint_lt,
            always_inline, and _if_else_0 at the call sites
"""
import io
import sys

variant, src = sys.argv[1], sys.argv[2]
BN = src + "/drivers/builtin/src/bignum_core.c"
CT = src + "/utilities/constant_time_impl.h"


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def sub1(text, old, new, what):
    n = text.count(old)
    if n != 1:
        sys.exit("PATCH ERROR: %s matched %d times (expected 1)" % (what, n))
    return text.replace(old, new)


SUB_CT = """        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(A[i], c),
                                                    1, 0);
        mbedtls_mpi_uint t = A[i] - c;
        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(t, B[i]), 1, 0) + z;"""

SUB_PLAIN = """        mbedtls_mpi_uint z = (A[i] < c);
        mbedtls_mpi_uint t = A[i] - c;
        c = (t < B[i]) + z;"""

SUB_FIX = """        mbedtls_mpi_uint z = mbedtls_ct_mpi_uint_if_else_0(
            mbedtls_ct_uint_lt(A[i], c), 1);
        mbedtls_mpi_uint t = A[i] - c;
        c = mbedtls_ct_mpi_uint_if_else_0(mbedtls_ct_uint_lt(t, B[i]), 1) + z;"""

MLA_CT = "        c = mbedtls_ct_mpi_uint_if(mbedtls_ct_uint_lt(*d, c), 1, 0);"
MLA_PLAIN = "        c = (*d < c);"
MLA_FIX = "        c = mbedtls_ct_mpi_uint_if_else_0(mbedtls_ct_uint_lt(*d, c), 1);"

CT_GENERIC_FIX2 = """#else
    /* Opaque the two inputs, then compute the borrow out of x - y with the
     * same six-operation sequence the Arm/AArch64/x86 asm paths implement
     * (eor / sub / bic / and / orr / asr).
     *
     * The barriers on the inputs are load-bearing: without them the optimiser
     * recognises the sequence, re-derives "x < y", and emits a conditional
     * branch (verified: clang 22 -O2 x86-64 does exactly that, and MemSan
     * constant-flow testing catches it). But TWO barriers suffice, where the
     * previous implementation used six -- two here plus one each in the
     * nested mbedtls_ct_bool(), mbedtls_ct_if() and the final ct_bool().
     * That matters on targets with no asm path (Xtensa, RISC-V, MIPS,
     * PowerPC), where each barrier also blocks inlining at -Os.
     */
    const mbedtls_ct_uint_t xo = mbedtls_ct_compiler_opaque(x);
    const mbedtls_ct_uint_t yo = mbedtls_ct_compiler_opaque(y);
    const mbedtls_ct_uint_t s_ = xo ^ yo;
    const mbedtls_ct_uint_t d_ = xo - yo;
    const mbedtls_ct_uint_t r_ = (d_ & ~s_) | (yo & s_);
    /* Broadcast the borrow bit: all bits set iff x < y. */
    return (mbedtls_ct_condition_t) (0u - (r_ >> (MBEDTLS_CT_SIZE - 1)));
"""

CT_GENERIC_NEW = """#else
    /* Branchless, barrier-free: the same six-operation sequence the Arm,
     * AArch64 and x86 paths above implement (eor / sub / bic / and / orr /
     * asr), written in C.
     *
     * There is no comparison and no branch here, so there is nothing for the
     * compiler to lower into a conditional jump, and hence no need for
     * mbedtls_ct_compiler_opaque(). Avoiding those barriers matters: they are
     * what stopped this function being inlined at -Os on targets with no asm
     * path (Xtensa, RISC-V, MIPS, PowerPC), turning every use into an
     * out-of-line call.
     */
    const mbedtls_ct_uint_t s_ = x ^ y;
    const mbedtls_ct_uint_t d_ = x - y;
    const mbedtls_ct_uint_t r_ = (d_ & ~s_) | (y & s_);
    /* Broadcast the borrow bit: all bits set iff x < y. */
    return (mbedtls_ct_condition_t) (0u - (r_ >> (MBEDTLS_CT_SIZE - 1)));
"""


def replace_generic_branch(text, body):
    """Replace the generic #else body of mbedtls_ct_uint_lt in place.

    Scoped to the function so the surrounding #endif/} still close it.
    """
    fn = "static inline mbedtls_ct_condition_t mbedtls_ct_uint_lt("
    i = text.find(fn)
    if i < 0:
        sys.exit("PATCH ERROR: mbedtls_ct_uint_lt not found")
    marker = "#else\n    /* Ensure that the compiler cannot optimise"
    j = text.find(marker, i)
    if j < 0:
        sys.exit("PATCH ERROR: generic #else branch not found")
    endmark = "    return mbedtls_ct_bool(ret);\n"
    k = text.find(endmark, j)
    if k < 0:
        sys.exit("PATCH ERROR: end of generic branch not found")
    return text[:j] + body + text[k + len(endmark):]

ALWAYS_OLD = ("static inline mbedtls_ct_condition_t "
              "mbedtls_ct_uint_lt(mbedtls_ct_uint_t x, mbedtls_ct_uint_t y)")
ALWAYS_NEW = ("""/* always_inline: at -Os GCC otherwise leaves this out of line, and on
 * Xtensa every use then costs a call plus a register-window entry and
 * spills. */
__attribute__((always_inline))
""" + ALWAYS_OLD)

bn, ct = rd(BN), rd(CT)

if variant == "stock":
    pass
elif variant == "revert":
    bn = sub1(bn, SUB_CT, SUB_PLAIN, "core_sub")
    bn = sub1(bn, MLA_CT, MLA_PLAIN, "core_mla")
elif variant == "fix":
    bn = sub1(bn, SUB_CT, SUB_FIX, "core_sub")
    bn = sub1(bn, MLA_CT, MLA_FIX, "core_mla")
    ct = replace_generic_branch(ct, CT_GENERIC_NEW)
    ct = sub1(ct, ALWAYS_OLD, ALWAYS_NEW, "always_inline attribute")
elif variant == "fix2":
    bn = sub1(bn, SUB_CT, SUB_FIX, "core_sub")
    bn = sub1(bn, MLA_CT, MLA_FIX, "core_mla")
    ct = replace_generic_branch(ct, CT_GENERIC_FIX2)
    ct = sub1(ct, ALWAYS_OLD, ALWAYS_NEW, "always_inline attribute")
elif variant == "callsites":
    # Only the arch-independent half of the proposed patch: _if_else_0 is
    # (condition & if1), so no ~condition and no barrier is needed. Must be
    # constant-flow clean on its own.
    bn = sub1(bn, SUB_CT, SUB_FIX, "core_sub")
    bn = sub1(bn, MLA_CT, MLA_FIX, "core_mla")
else:
    sys.exit("unknown variant " + variant)

wr(BN, bn)
wr(CT, ct)
print("applied variant: " + variant)
