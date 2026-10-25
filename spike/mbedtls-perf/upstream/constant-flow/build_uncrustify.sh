#!/bin/bash
# Build the exact uncrustify the project pins (0.75.1) from the official
# upstream repository. Ubuntu ships 0.78.1, which code_style.py refuses, and
# running a different formatter version over the file could introduce
# reformatting CI would then reject.
#
# Nothing is installed system-wide; the binary stays under $HOME.
set -e
ROOT="$HOME/uncrustify-build"
BIN="$ROOT/uncrustify/build/uncrustify"

if [ -x "$BIN" ]; then
  echo "already built: $("$BIN" --version)"
  exit 0
fi

mkdir -p "$ROOT"
cd "$ROOT"
if [ ! -d uncrustify/.git ]; then
  git clone --depth 1 --branch uncrustify-0.75.1 \
    https://github.com/uncrustify/uncrustify.git uncrustify 2>&1 | tail -2
fi
cd uncrustify
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -30 make.log; exit 1; }
echo "built: $(./uncrustify --version)"
