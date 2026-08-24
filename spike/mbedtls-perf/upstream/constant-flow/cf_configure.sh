#!/bin/bash
# Configure TF-PSA-Crypto for MemSan constant-flow testing.
#
# Lives under $HOME, not /tmp: systemd-tmpfiles clears /tmp when the WSL
# instance restarts, and it restarts between tool calls.
#
# MBEDTLS_HAVE_ASM is deliberately UNSET. With it set, mbedtls_ct_uint_lt()
# takes the x86-64 assembly path on this host, which is NOT the code under
# test. Unsetting it forces the generic C branch -- the one Xtensa, RISC-V,
# MIPS and PowerPC use, and the one the patch changes.
set -e
ROOT="$HOME/ctflow"
SRC="$ROOT/tfpsa"

if [ ! -d "$SRC/.git" ]; then
  echo "=== cloning TF-PSA-Crypto main (with framework submodule) ==="
  rm -rf "$ROOT"
  mkdir -p "$ROOT"
  git clone --recursive --depth 1 \
    https://github.com/Mbed-TLS/TF-PSA-Crypto.git "$SRC" 2>&1 | tail -2
fi

cd "$SRC"
git stash list >/dev/null 2>&1 || true
git checkout -- . 2>/dev/null || true
echo "=== upstream commit under test ==="
git log -1 --format='%H  %ad  %s'

python3 scripts/config.py set MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN
python3 scripts/config.py unset MBEDTLS_HAVE_ASM
# These hard-#error without asm/intrinsics, and are irrelevant to bignum.
python3 scripts/config.py unset MBEDTLS_AESNI_C
python3 scripts/config.py unset MBEDTLS_AESCE_C
python3 scripts/config.py unset MBEDTLS_PADLOCK_C

echo "=== config assertions ==="
grep -n '^#define MBEDTLS_TEST_CONSTANT_FLOW_MEMSAN' include/psa/crypto_config.h \
  || echo 'FAIL: CONSTANT_FLOW_MEMSAN not set'
if grep -q '^#define MBEDTLS_HAVE_ASM' include/psa/crypto_config.h; then
  echo 'FAIL: MBEDTLS_HAVE_ASM still set - would test the x86 asm path'
else
  echo 'OK: MBEDTLS_HAVE_ASM unset -> generic C ct_uint_lt is under test'
fi

echo "=== cmake configure (clang, MemSanDbg) ==="
rm -rf build-cf
mkdir build-cf
cd build-cf
CC=clang cmake -DCMAKE_BUILD_TYPE=MemSanDbg .. > cmake.log 2>&1 || {
  echo "CMAKE FAILED"; tail -40 cmake.log; exit 1; }
echo "cmake OK"

echo "=== bignum_core test targets ==="
make help 2>/dev/null | grep -i 'bignum_core' | head
