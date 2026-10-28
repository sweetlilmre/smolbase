#!/usr/bin/env python3
"""Tripwire for the two upstream PRs this project patches into the SDK.

`cmake/upstream_patches.cmake` applies two patch series to `$IDF_PATH` at
configure time, and that build already fails loudly when a patch stops
applying. This script answers the question that FATAL cannot: *has the PR
actually merged into the SDK we use, and what else has to change when it does?*

The distinction matters because the two PRs merge in two different places and
only one of them is visible from here:

  - Mbed-TLS/TF-PSA-Crypto#873 merged upstream on 2026-10-28 (9088518a). That
    is TF-PSA-Crypto's own `development` branch. ESP-IDF does not consume it
    directly — it consumes espressif/mbedtls, a fork, which vendors
    TF-PSA-Crypto under tf-psa-crypto/. So "merged" upstream and "arrived in
    the SDK" are months apart, and only the second one is actionable.
  - espressif/esp-idf#19027 is approved and PR-Sync-Merge pinned, not merged.

Neither can arrive by itself, because the SDK is PINNED: v6.0.2 locally and in
CI. Time passing changes nothing. The trigger is *us moving the pin* — or, for
the scheduled remote check, the fork publishing a branch that carries the code.

Detection is by CONTENT, not by commit SHA, because the fork rebases and
re-tags and a SHA would not survive that. Each marker was chosen to be absent
from the current pinned tree and present once the PR lands:

  #873    MBEDTLS_CT_XTENSA_ASM       0 -> 5 occurrences
  #19027  ESP_MPI_HW_MIN_BIT_LEN      0 -> 24 occurrences

Local mode reads COMMITTED content (`git show HEAD:path`), never the working
tree. The working tree is where our own patches live, so grepping it would
report every patched checkout as a merge.

Usage:
    uv run scripts/check_upstream_merges.py            # local, needs IDF_PATH
    uv run scripts/check_upstream_merges.py --remote   # GitHub, needs gh auth
    uv run scripts/check_upstream_merges.py --both

Exit status:
    0  neither has landed - patches still required, nothing to do
    1  at least one has landed - see smolbase#134, the table needs re-measuring
    2  the check itself could not run (no IDF_PATH, no gh, path moved)

A nonzero exit is the whole point: the scheduled CI job in
.github/workflows/upstream-watch.yml fails, and a failing scheduled workflow
mails the repo owner. That is the notification nobody has to remember to check.
"""
import argparse
import base64
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field

ISSUE = "https://github.com/sweetlilmre/smolbase/issues/134"


@dataclass
class Marker:
    """One upstream PR, and how to tell whether its code is present."""

    pr: str
    url: str
    # Where the code lands locally: path relative to a repo under IDF_PATH.
    local_repo: str  # "" means $IDF_PATH itself
    local_path: str
    # Where to look for it on GitHub, and which refs are worth asking about.
    remote_repo: str
    remote_path: str
    remote_refs: list[str]
    # Branch-name prefix to discover newest-first at run time, so the check
    # does not go blind the day the fork publishes a version this list has
    # never heard of. "" disables discovery and uses remote_refs alone.
    ref_prefix: str
    needle: str
    # What the pinned v6.0.2 tree reports today, so a change in either
    # direction is visible rather than only a 0 -> nonzero transition.
    baseline: int = 0
    consequences: list[str] = field(default_factory=list)


MARKERS = [
    Marker(
        pr="TF-PSA-Crypto#873",
        url="https://github.com/Mbed-TLS/TF-PSA-Crypto/pull/873",
        local_repo="components/mbedtls/mbedtls",
        local_path="tf-psa-crypto/utilities/constant_time_impl.h",
        remote_repo="espressif/mbedtls",
        remote_path="tf-psa-crypto/utilities/constant_time_impl.h",
        # The fork's release branches are named mbedtls-<version>-idf, and
        # they are the only place to look: its `master` predates the 4.x
        # split and has no tf-psa-crypto/ directory at all. Discovered rather
        # than listed, because the branch that eventually carries #873 does
        # not exist yet and cannot be named here.
        remote_refs=[],
        ref_prefix="mbedtls-",
        needle="MBEDTLS_CT_XTENSA_ASM",
        consequences=[
            "Delete patches/0001-, 0002-, 0003- and their sb_apply_series call.",
            "RE-MEASURE the #19027 threshold table. #873 speeds the software",
            "multiply 6-12%, which lifts the crossover ~5% and flips 384-bit",
            "operands to the software side. The <=80 MHz rows route 384 to",
            "HARDWARE and become the losing choice by ~5% on both parts.",
            "Until re-measured, CONFIG_MBEDTLS_MPI_HW_MIN_BIT_LEN overrides it.",
        ],
    ),
    Marker(
        pr="esp-idf#19027",
        url="https://github.com/espressif/esp-idf/pull/19027",
        local_repo="",
        local_path="components/mbedtls/port/bignum/esp_bignum.c",
        remote_repo="espressif/esp-idf",
        remote_path="components/mbedtls/port/bignum/esp_bignum.c",
        # IDF's own branches are stable names, and release/v6.x is where a
        # backport would appear, so these are listed rather than discovered.
        remote_refs=["master", "release/v6.0"],
        ref_prefix="",
        needle="ESP_MPI_HW_MIN_BIT_LEN",
        consequences=[
            "Delete the four patches/esp-idf-mpi-*.patch and their series call.",
            "Keep CONFIG_MBEDTLS_HARDWARE_MPI=y - the merge is what licenses it.",
            "Check whether the merged table matches patch 4; the PR branch and",
            "the local fourth patch can diverge if review changed the values.",
        ],
    ),
]


def run(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout


def count_local(m: Marker, idf_path: str) -> int | None:
    """Occurrences in the SDK's COMMITTED content, or None if unreadable."""
    repo = os.path.join(idf_path, m.local_repo) if m.local_repo else idf_path
    rc, out = run(["git", "-C", repo, "show", f"HEAD:{m.local_path}"])
    if rc != 0:
        return None
    return out.count(m.needle)


def discover_refs(m: Marker, keep: int = 3) -> list[str]:
    """The newest `keep` branches matching m.ref_prefix, newest first.

    Sorted on the version numbers embedded in the name, not lexically:
    mbedtls-4.10.0-idf has to sort above mbedtls-4.9.0-idf, and a string sort
    puts it below. Names that carry no version sort last rather than crashing
    the check, since a tripwire that dies on an unexpected branch name is
    worse than one that merely looks at the wrong branch.
    """
    if not m.ref_prefix:
        return []
    rc, out = run(["gh", "api", f"repos/{m.remote_repo}/branches",
                   "--paginate", "--jq", ".[].name"])
    if rc != 0:
        return []

    def version(name: str) -> tuple:
        digits = [int(p) for p in re.findall(r"\d+", name)]
        return (len(digits) > 0, digits)

    names = [n for n in out.split() if n.startswith(m.ref_prefix)]
    return sorted(names, key=version, reverse=True)[:keep]


def count_remote(m: Marker, ref: str) -> int | None:
    """Occurrences on a GitHub ref, or None if the ref or path is absent."""
    rc, out = run(
        ["gh", "api", f"repos/{m.remote_repo}/contents/{m.remote_path}?ref={ref}"]
    )
    if rc != 0:
        return None
    try:
        blob = json.loads(out)["content"]
    except (json.JSONDecodeError, KeyError):
        return None
    return base64.b64decode(blob).decode("utf-8", "replace").count(m.needle)


def report(m: Marker, where: str, n: int | None) -> bool:
    """Print one line. True if the PR appears to have landed there."""
    if n is None:
        print(f"  {where:<24} ?  unreadable (path moved? ref gone?)")
        return False
    landed = n > m.baseline
    print(f"  {where:<24} {'LANDED' if landed else 'absent'}  ({n} x {m.needle})")
    return landed


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--remote", action="store_true", help="check GitHub refs")
    ap.add_argument("--both", action="store_true", help="check local and GitHub")
    args = ap.parse_args()
    do_local = args.both or not args.remote
    do_remote = args.both or args.remote

    idf_path = os.environ.get("IDF_PATH", "")
    if do_local and not idf_path:
        print("IDF_PATH is not set; run the SDK export script, or use --remote.")
        return 2
    if do_remote and run(["gh", "auth", "status"])[0] != 0:
        print("gh is not authenticated; --remote needs it.")
        return 2

    landed = []
    for m in MARKERS:
        print(f"\n{m.pr}  {m.url}")
        hit = False
        if do_local:
            _, tag = run(["git", "-C", idf_path, "describe", "--tags"])
            hit |= report(m, f"local {tag.strip() or '?'}", count_local(m, idf_path))
        if do_remote:
            for ref in discover_refs(m) + m.remote_refs:
                hit |= report(m, f"{m.remote_repo.split('/')[1]}@{ref}",
                              count_remote(m, ref))
        if hit:
            landed.append(m)

    if not landed:
        print("\nNeither PR has landed in the SDK. Patches still required.")
        return 0

    print("\n" + "=" * 72)
    print("ACTION REQUIRED - an upstream PR has landed in the SDK")
    print("=" * 72)
    for m in landed:
        print(f"\n{m.pr}:")
        for line in m.consequences:
            print(f"  {line}")
    print(f"\nFull context and the revert protocol: {ISSUE}")
    print("See also patches/README.md.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
