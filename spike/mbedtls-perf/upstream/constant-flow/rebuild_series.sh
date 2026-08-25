#!/bin/bash
# Rebuild the three-commit series from scratch, running the project's own
# code_style.py --fix after each stage so the formatting the checker wants is
# baked into the commit that introduces the code, not bolted on afterwards.
set -e
REPO=/mnt/d/source/TF-PSA-Crypto
GEN=/mnt/d/source/smolbase/spike/mbedtls-perf/upstream/gen_patches.py
PATCHDIR=/mnt/d/source/smolbase/spike/mbedtls-perf/upstream
UNC="$HOME/uncrustify-build/uncrustify/build/uncrustify"

cd "$REPO"
BASE=$(git rev-parse development)
echo "resetting branch to development ($BASE)"
git reset -q --hard "$BASE"

declare -A NAMES=(
  [1]=0001-ct-force-inline-asm-paths
  [2]=0002-ct-xtensa-asm-path
  [3]=0003-bignum-core-if-else-0
)

for S in 1 2 3; do
  NAME=${NAMES[$S]}
  echo "=== stage $S : $NAME ==="
  python3 "$GEN" "$S" "$REPO" > /dev/null

  # The project's own checker, in fix mode, with the version it pins.
  python3 framework/scripts/code_style.py --fix --uncrustify "$UNC" > /tmp/fix.log 2>&1 || true
  echo "  files touched after style fix:"
  git status --porcelain | sed 's/^/    /'

  # Commit message: the header of the corresponding patch file, above the '---'.
  python3 - "$PATCHDIR/$NAME.patch" > /tmp/msg.txt <<'PY'
import io, sys
h = io.open(sys.argv[1], encoding="utf-8").read().split("\n---\n", 1)[0]
lines = h.split("\n")
subj = None; start = None
for i, l in enumerate(lines):
    if l.startswith("Subject: "):
        subj = l[len("Subject: "):]
        if subj.startswith("[") and "] " in subj:
            subj = subj.split("] ", 1)[1]
        start = i + 1
        break
body = "\n".join(lines[start:]).strip("\n")
sys.stdout.write(subj + "\n\n" + body + "\n")
PY
  git add -A
  git commit -q -s -F /tmp/msg.txt
  echo "  committed: $(git log -1 --format=%s)"
done

echo
echo "=== final style check on the finished series ==="
if python3 framework/scripts/code_style.py --uncrustify "$UNC" 2>&1 | tail -5 | grep -q 'incorrect'; then
  echo "  STYLE STILL INCORRECT"
else
  echo "  STYLE CLEAN"
fi
echo
git log --oneline development..HEAD
