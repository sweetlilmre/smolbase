#!/bin/bash
# Does always_inline fix the Arm -Os out-of-line call, and at what size cost?
# Also checks the generic C path, which every non-asm target uses.
set -e
SRC="$HOME/ctflow/tfpsa"
cd "$SRC"
INCS=$(grep -o '\-I[^ ]*' build-genc-gcc-Os/drivers/builtin/CMakeFiles/builtin.dir/flags.make 2>/dev/null \
       | sort -u | tr '\n' ' ')

probe () {   # $1 = label
  OUT=/tmp/bn_arm.o
  arm-none-eabi-gcc -mthumb -mcpu=cortex-m4 -Os -c \
    drivers/builtin/src/bignum_core.c -o "$OUT" $INCS 2>/tmp/e.log || {
      echo "  $1: compile failed"; head -3 /tmp/e.log; return; }
  CALLS=$(arm-none-eabi-objdump -d --disassemble='mbedtls_mpi_core_sub' "$OUT" \
          | grep -cE '\b(bl|blx)\b.*mbedtls_ct_uint_lt' || true)
  SUBSZ=$(arm-none-eabi-nm -S "$OUT" | awk '$4=="mbedtls_mpi_core_sub"{print strtonum("0x"$2)}')
  TEXT=$(arm-none-eabi-size "$OUT" | awk 'NR==2{print $1}')
  printf "  %-34s calls=%-3s core_sub=%-4s bytes  .text=%s bytes\n" "$1" "$CALLS" "$SUBSZ" "$TEXT"
}

echo "=== Arm thumb cortex-m4, -Os ==="
git checkout -- .
probe "stock (asm path, no attribute)"

git checkout -- .
python3 - <<'PY'
import io
p="utilities/constant_time_impl.h"
s=io.open(p,encoding="utf-8").read()
for sig in ["static inline mbedtls_ct_condition_t mbedtls_ct_bool(mbedtls_ct_uint_t x)\n{",
            "static inline mbedtls_ct_uint_t mbedtls_ct_if(mbedtls_ct_condition_t condition,",
            "static inline mbedtls_ct_condition_t mbedtls_ct_uint_lt(mbedtls_ct_uint_t x, mbedtls_ct_uint_t y)\n{"]:
    assert s.count(sig)==1, sig[:60]
    s=s.replace(sig, "__attribute__((always_inline))\n"+sig)
io.open(p,"w",encoding="utf-8",newline="").write(s)
PY
probe "asm path + always_inline"

echo "=== generic C path (what non-asm targets use), Arm -Os ==="
git checkout -- .
python3 scripts/config.py unset MBEDTLS_HAVE_ASM
probe "generic C, no attribute"

python3 - <<'PY'
import io
p="utilities/constant_time_impl.h"
s=io.open(p,encoding="utf-8").read()
for sig in ["static inline mbedtls_ct_condition_t mbedtls_ct_bool(mbedtls_ct_uint_t x)\n{",
            "static inline mbedtls_ct_uint_t mbedtls_ct_if(mbedtls_ct_condition_t condition,",
            "static inline mbedtls_ct_condition_t mbedtls_ct_uint_lt(mbedtls_ct_uint_t x, mbedtls_ct_uint_t y)\n{"]:
    s=s.replace(sig, "__attribute__((always_inline))\n"+sig)
io.open(p,"w",encoding="utf-8",newline="").write(s)
PY
probe "generic C + always_inline"
git checkout -- .
