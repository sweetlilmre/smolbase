# PlatformIO extra_script: assembles data/w/ from html/ as gzip-only assets.
# Only the .gz copies ship: PsychicHttp auto-serves name.gz (with the gzip
# content-encoding header) when name is requested. Skips files whose gz is
# already newer than the source.
# NB: keep the literal string "coding:"-like patterns out of the first two lines —
# Python's PEP 263 encoding sniffer reads them and one comment here once broke the build.
import gzip
from pathlib import Path

Import("env")  # noqa: F821 — provided by SCons

PROJECT = Path(env["PROJECT_DIR"])  # noqa: F821
SRC = PROJECT / "html"
DST = PROJECT / "data" / "w"


def pack() -> None:
    if not SRC.is_dir():
        return
    DST.mkdir(parents=True, exist_ok=True)
    expected = set()
    for f in SRC.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(SRC)
        out = DST / rel.parent / (rel.name + ".gz")
        expected.add(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists() and out.stat().st_mtime >= f.stat().st_mtime:
            continue
        # mtime=0 keeps the gzip output deterministic for identical content
        out.write_bytes(gzip.compress(f.read_bytes(), 9, mtime=0))
        print(f"packed {rel} -> {out.relative_to(PROJECT)} ({out.stat().st_size} B)")
    # Mirror-sync: data/ is gitignored and persists across checkouts, so a
    # deleted or renamed html/ file would otherwise keep shipping its stale
    # .gz in every locally built filesystem image, forever.
    for old in DST.rglob("*.gz"):
        if old not in expected:
            old.unlink()
            print(f"pruned stale {old.relative_to(PROJECT)}")


pack()
