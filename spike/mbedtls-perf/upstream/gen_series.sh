#!/bin/bash
# Generate the three-patch series as incremental diffs against upstream main,
# then verify the series applies to a pristine checkout in order, and that
# patch 3 also applies on its own.
set -e
SRC="$HOME/ctflow/tfpsa"
OUT="/mnt/d/source/smolbase/spike/mbedtls-perf/upstream"
GEN="/mnt/c/Users/petere/AppData/Local/Temp/claude/D--source-smolbase/505db696-ca1a-42fe-83cb-f1c80cb331b2/scratchpad/gen_patches.py"

cd "$SRC"
git checkout -- .
git stash list >/dev/null 2>&1 || true

for S in 1 2 3; do
  python3 "$GEN" "$S" "$SRC" > /dev/null
  git diff > "$OUT/stage$S.body"
  git add -A
  git -c user.email=x@y -c user.name=x commit -qm "stage $S"
done
git reset -q --hard HEAD~3   # back to pristine upstream

echo "=== generated bodies ==="
wc -l "$OUT"/stage*.body

echo "=== series applies in order to a pristine tree? ==="
git checkout -- .
OK=yes
for S in 1 2 3; do
  if git apply "$OUT/stage$S.body"; then echo "  stage $S: applied"; else echo "  stage $S: FAILED"; OK=no; fi
done
git checkout -- .

echo "=== patch 3 applies on its own (no dependency on 1 or 2)? ==="
git apply --check "$OUT/stage3.body" && echo "  yes" || echo "  no"
echo "=== patch 1 applies on its own? ==="
git apply --check "$OUT/stage1.body" && echo "  yes" || echo "  no"
echo "=== patch 2 applies on its own? ==="
git apply --check "$OUT/stage2.body" && echo "  yes" || echo "  no"
git checkout -- .
echo "series verification: $OK"
