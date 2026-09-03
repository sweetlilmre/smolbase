#!/bin/bash
# Does the compiler inline mbedtls_ct_uint_lt into mbedtls_mpi_core_sub on
# architectures that DO have an assembly path?
#
# This is the question the Xtensa result raises: if Arm/x86 also leave it out
# of line, the out-of-line-call cost is general and not an Xtensa quirk, and
# adding always_inline for Xtensa alone would be treating a symptom.
#
# Stock upstream source, no patch. MBEDTLS_HAVE_ASM left at its default (set),
# so x86-64 takes MBEDTLS_CT_X86_64_ASM.
set -e
SRC="$HOME/ctflow/tfpsa"
cd "$SRC"
git checkout -- . 2>/dev/null || true
python3 scripts/config.py unset MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN 2>/dev/null || true

echo "=== confirming x86-64 takes the asm path ==="
grep -q '^#define MBEDTLS_HAVE_ASM' include/psa/crypto_config.h \
  && echo "  MBEDTLS_HAVE_ASM set -> MBEDTLS_CT_X86_64_ASM in use" \
  || echo "  WARNING: HAVE_ASM unset, generic C would be tested instead"

for CC in gcc clang; do
  for OPT in -Os -O2; do
    D="build-probe-$CC$OPT"
    rm -rf "$D"; mkdir "$D"; cd "$D"
    CC=$CC cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$OPT" .. \
      > cm.log 2>&1 || { echo "cmake failed ($CC $OPT)"; tail -5 cm.log; cd ..; continue; }
    make -j"$(nproc)" builtin > mk.log 2>&1 || {
      echo "build failed ($CC $OPT)"; grep -m2 -A3 error mk.log; cd ..; continue; }
    OBJ=$(find . -name 'bignum_core.c.o' | head -1)
    CALLS=$(objdump -d --disassemble='mbedtls_mpi_core_sub' "$OBJ" 2>/dev/null \
            | grep -c 'call.*mbedtls_ct_uint_lt' || true)
    INSNS=$(objdump -d --disassemble='mbedtls_mpi_core_sub' "$OBJ" 2>/dev/null \
            | grep -cE '^\s+[0-9a-f]+:' || true)
    SYM=$(nm "$OBJ" | grep -c 'mbedtls_ct_uint_lt' || true)
    printf "  %-6s %-4s : core_sub insns=%-4s calls-to-ct_uint_lt=%-3s ct_uint_lt-symbol-present=%s\n" \
      "$CC" "$OPT" "$INSNS" "$CALLS" "$SYM"
    cd ..
  done
done
