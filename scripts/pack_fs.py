#!/usr/bin/env python3
# Assembles an App's data dir from html/ as gzip-only assets.
#
# Per-App overlay: the App's own html/ directory is merged on top of the shared
# html/ and takes precedence (same relative path = overlay wins), so each App
# ships its own index.html without touching the shared base assets.
# Only .gz copies ship: PsychicHttp auto-serves name.gz with the right
# content-encoding header when name is requested.
# Each App packs into its OWN data dir (data-<app>/) so filesystem images and
# asset tars contain exactly that App's files — a shared data/ let one App's
# assets leak into another's image (wayfinder #124).
#
# Called from the root CMakeLists.txt (target `smolbase_assets`, a dependency of
# the littlefs image) and from CI. It used to be a PlatformIO extra_script
# importing SCons's `env`; it is now a plain CLI so the build system it serves
# can be CMake.
#
#   python scripts/pack_fs.py --app weatherclock
#   python scripts/pack_fs.py --all
#
# NB: keep the literal string "coding:"-like patterns out of the first two lines —
# Python's PEP 263 encoding sniffer reads them and one comment here once broke the build.
import argparse
import gzip
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent

# App name -> its html overlay directory. The App names are the ones the release
# assets and the docs use; the source directories do not match them one-for-one
# (historical). The same mapping lives in the root CMakeLists.txt for SRC_DIRS.
APPS = {
    "smolbase": "src/app-smolbase/html",
    "weatherclock": "src/app-weather/html",
    "gcm": "src/app-gcm/html",
}

BASE_SRC = PROJECT / "html"


def pack(app: str) -> None:
    app_src = PROJECT / APPS[app]
    dst = PROJECT / f"data-{app}" / "w"

    if not BASE_SRC.is_dir():
        print(f"pack_fs: no {BASE_SRC.relative_to(PROJECT)} directory — nothing to pack")
        return
    dst.mkdir(parents=True, exist_ok=True)

    # Collect files: base first, then the App overlay (overlay wins on conflict).
    files: dict[Path, Path] = {}
    for src_dir in (BASE_SRC, app_src):
        if not src_dir.is_dir():
            continue
        for f in src_dir.rglob("*"):
            if f.is_file():
                files[f.relative_to(src_dir)] = f

    expected = set()
    for rel, src_file in sorted(files.items()):
        out = dst / rel.parent / (rel.name + ".gz")
        expected.add(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists() and out.stat().st_mtime >= src_file.stat().st_mtime:
            continue
        # mtime=0 keeps the gzip output deterministic for identical content
        out.write_bytes(gzip.compress(src_file.read_bytes(), 9, mtime=0))
        print(f"packed {src_file.relative_to(PROJECT)} -> {out.relative_to(PROJECT)} "
              f"({out.stat().st_size} B)")

    # Mirror-sync: data dirs are gitignored and persist across checkouts, so a
    # deleted or renamed source file would otherwise keep shipping its stale .gz
    # in every locally built filesystem image, forever. The data dir is per-App,
    # so anything not expected for THIS App is stale by definition.
    for old in dst.rglob("*.gz"):
        if old not in expected:
            old.unlink()
            print(f"pruned stale {old.relative_to(PROJECT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--app", choices=sorted(APPS), help="App to pack assets for")
    ap.add_argument("--all", action="store_true", help="pack every App")
    args = ap.parse_args()

    if args.all:
        for app in sorted(APPS):
            pack(app)
    elif args.app:
        pack(args.app)
    else:
        ap.error("give --app <name> or --all")
    return 0


if __name__ == "__main__":
    sys.exit(main())
