# PlatformIO extra_script: assembles data/w/ from html/ as gzip-only assets.
# Per-env overlay: the env's custom_app_html directory is merged on top of
# html/ and takes precedence (same relative path = overlay wins). This lets
# each app ship its own index.html without touching the shared base assets.
# Only .gz copies ship: PsychicHttp auto-serves name.gz with the correct
# content-encoding header when name is requested.
# NB: keep the literal string "coding:"-like patterns out of the first two lines —
# Python's PEP 263 encoding sniffer reads them and one comment here once broke the build.
import gzip
from pathlib import Path

Import("env")  # noqa: F821 — provided by SCons

PROJECT  = Path(env["PROJECT_DIR"])  # noqa: F821
BASE_SRC = PROJECT / "html"
APP_SRC  = PROJECT / env.GetProjectOption("custom_app_html", "")  # noqa: F821
DST      = PROJECT / "data" / "w"


def pack() -> None:
    if not BASE_SRC.is_dir():
        return
    DST.mkdir(parents=True, exist_ok=True)

    # Collect files: base first, then per-env overlay (overlay wins on conflict).
    files: dict = {}
    for src_dir in (BASE_SRC, APP_SRC):
        if not src_dir.is_dir():
            continue
        for f in src_dir.rglob("*"):
            if f.is_file():
                files[f.relative_to(src_dir)] = f

    expected = set()
    for rel, src_file in files.items():
        out = DST / rel.parent / (rel.name + ".gz")
        expected.add(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists() and out.stat().st_mtime >= src_file.stat().st_mtime:
            continue
        # mtime=0 keeps the gzip output deterministic for identical content
        out.write_bytes(gzip.compress(src_file.read_bytes(), 9, mtime=0))
        src_label = src_file.relative_to(PROJECT)
        print(f"packed {src_label} -> {out.relative_to(PROJECT)} ({out.stat().st_size} B)")

    # Mirror-sync: data/ is gitignored and persists across checkouts, so a
    # deleted or renamed source file would otherwise keep shipping its stale
    # .gz in every locally built filesystem image, forever.
    # Build the union of files reachable from ALL app html dirs so that one
    # env's prune pass doesn't delete a gz that belongs to a different env.
    all_app_srcs = list(PROJECT.glob("src/app-*/html"))
    union_expected = set(expected)
    for app_src in all_app_srcs:
        if app_src == APP_SRC:
            continue
        for f in app_src.rglob("*"):
            if f.is_file():
                union_expected.add(DST / f.relative_to(app_src).parent / (f.name + ".gz"))
    for old in DST.rglob("*.gz"):
        if old not in union_expected:
            old.unlink()
            print(f"pruned stale {old.relative_to(PROJECT)}")


pack()
