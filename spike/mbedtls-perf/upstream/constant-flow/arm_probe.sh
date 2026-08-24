#!/bin/bash
# Does Arm (which HAS an asm path and never needed always_inline) inline
# mbedtls_ct_uint_lt into mbedtls_mpi_core_sub at -Os?
#
# Compiles the single translation unit rather than the whole library: all we
# need is whether the call survives.
set -e
SRC="$HOME/ctflow/tfpsa"
cd "$SRC"
git checkout -- .            # restore stock config (MBEDTLS_HAVE_ASM set)

# Reuse the include flags cmake worked out for the host build.
INCS=$(grep -o '\-I[^ ]*' build-genc-gcc-Os/drivers/builtin/CMakeFiles/builtin.dir/flags.make 2>/dev/null \
       | sort -u | tr '\n' ' ')
if [ -z "$INCS" ]; then
  INCS="-I include -I drivers/builtin/include -I drivers/builtin/src -I utilities -I core -I ."
fi
echo "include flags: $(echo $INCS | head -c 200)..."

for MODE in "-mthumb -mcpu=cortex-m4" "-marm -mcpu=arm7tdmi"; do
  for OPT in -Os -O2; do
    OUT=/tmp/bn_arm.o
    if arm-none-eabi-gcc $MODE $OPT -c drivers/builtin/src/bignum_core.c -o "$OUT" \
         $INCS 2>/tmp/arm_err.log; then
      CALLS=$(arm-none-eabi-objdump -d --disassemble='mbedtls_mpi_core_sub' "$OUT" \
              | grep -cE '\b(bl|blx)\b.*mbedtls_ct_uint_lt' || true)
      SYM=$(arm-none-eabi-nm "$OUT" | grep -c 'mbedtls_ct_uint_lt' || true)
      INSNS=$(arm-none-eabi-objdump -d --disassemble='mbedtls_mpi_core_sub' "$OUT" \
              | grep -cE '^\s+[0-9a-f]+:' || true)
      printf "  %-22s %-4s : insns=%-4s calls-to-ct_uint_lt=%-3s symbol=%s\n" \
        "$MODE" "$OPT" "$INSNS" "$CALLS" "$SYM"
    else
      echo "  $MODE $OPT : compile failed"
      head -4 /tmp/arm_err.log
    fi
  done
done
