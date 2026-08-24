#!/bin/bash
# Restore pristine sources, apply one variant, then run the constant-flow proof.
# $1 = variant (stock | revert | fix)
set -e
VARIANT="$1"
SRC="$HOME/ctflow/tfpsa"
PRIS="$HOME/ctflow/pristine"
PATCHER="/mnt/c/Users/petere/AppData/Local/Temp/claude/D--source-smolbase/505db696-ca1a-42fe-83cb-f1c80cb331b2/scratchpad/cf_patch.py"

mkdir -p "$PRIS"
for REL in drivers/builtin/src/bignum_core.c utilities/constant_time_impl.h; do
  BASE=$(basename "$REL")
  if [ ! -f "$PRIS/$BASE" ]; then
    cp "$SRC/$REL" "$PRIS/$BASE"
  fi
  cp "$PRIS/$BASE" "$SRC/$REL"
done

python3 "$PATCHER" "$VARIANT" "$SRC"
