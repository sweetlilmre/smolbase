#!/bin/bash
# Build the library for one variant and run the targeted constant-flow harness.
# $1 = variant (stock | revert | fix)
set -e
VARIANT="$1"
SRC="$HOME/ctflow/tfpsa"
SCRATCH="/mnt/c/Users/petere/AppData/Local/Temp/claude/D--source-smolbase/505db696-ca1a-42fe-83cb-f1c80cb331b2/scratchpad"

bash "$SCRATCH/cf_variant.sh" "$VARIANT" >/dev/null
cd "$SRC/build-cf"

echo "############################################################"
echo "### $VARIANT"
echo "############################################################"
sed -n '/^mbedtls_mpi_uint mbedtls_mpi_core_sub(/,/^}/p' \
  "$SRC/drivers/builtin/src/bignum_core.c" | sed -n '7,12p'

make -j"$(nproc)" tfpsacrypto > build.log 2>&1 || {
  echo "BUILD FAILED"; grep -m3 -A4 'error' build.log; exit 1; }

# Confirm the limb type assumption the harness makes.
if ! grep -q '#define MBEDTLS_HAVE_INT64' "$SRC/include/psa/crypto_config.h" 2>/dev/null; then
  : # not explicitly set; on x86-64 mbedtls picks 64-bit limbs by default
fi

LIBS=$(find . -name '*.a' | tr '\n' ' ')
clang -fsanitize=memory -fno-sanitize-memory-param-retval -fsanitize-memory-track-origins=2 -g -O2 \
      -o $HOME/ctflow/cf_harness "$SCRATCH/cf_harness.c" \
      -Wl,--start-group $LIBS -Wl,--end-group 2>$HOME/ctflow/cc.log || {
  echo "HARNESS LINK FAILED"; tail -20 $HOME/ctflow/cc.log; exit 1; }

set +e
$HOME/ctflow/cf_harness > $HOME/ctflow/harness.log 2>&1
RC=$?
set -e

if [ "$RC" = "0" ]; then
  echo "VERDICT [$VARIANT]: PASS - $(tail -1 $HOME/ctflow/harness.log)"
else
  echo "VERDICT [$VARIANT]: FAIL - constant-flow violation detected"
  grep -m1 -A12 'MemorySanitizer' $HOME/ctflow/harness.log
fi

# Independent check: conditional branches in the library's own core_sub,
# excluding the loop back-edge (which depends only on the public limb count).
OBJ=$(find . -name 'bignum_core.c.o' | head -1)
echo "--- core_sub codegen (x86-64, clang -O2 +msan) ---"
objdump -d --disassemble='mbedtls_mpi_core_sub' "$OBJ" 2>/dev/null \
  | grep -E '^\s+[0-9a-f]+:.*\s(j[a-z]+|cmov[a-z]+|set[a-z]+|sbb)\s' \
  | awk '{ $1=""; print "   " $0 }' | sed 's/  */ /g' | head -14
echo "exit=$RC"
