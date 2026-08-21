# PlatformIO extra_script: assembles the env's data dir from html/ as
# gzip-only assets. Per-env overlay: the env's custom_app_html directory is
# merged on top of html/ and takes precedence (same relative path = overlay
# wins). This lets each app ship its own index.html without touching the
# shared base assets.
# Only .gz copies ship: PsychicHttp auto-serves name.gz with the correct
# content-encoding header when name is requested.
# Each env packs into its OWN data dir (data-<env>/, PROJECT_DATA_DIR is
# redirected below) so filesystem images and asset tars contain exactly that
# env's files — the old shared data/ let one env's assets leak into another
# env's image (wayfinder #124).
# NB: keep the literal string "coding:"-like patterns out of the first two lines —
# Python's PEP 263 encoding sniffer reads them and one comment here once broke the build.
import gzip
from pathlib import Path

Import("env")  # noqa: F821 — provided by SCons

PROJECT  = Path(env["PROJECT_DIR"])  # noqa: F821
BASE_SRC = PROJECT / "html"
APP_SRC  = PROJECT / env.GetProjectOption("custom_app_html", "")  # noqa: F821
DATA_DIR = PROJECT / ("data-" + env["PIOENV"])  # noqa: F821
DST      = DATA_DIR / "w"

# Point the buildfs target at this env's data dir.
env.Replace(PROJECT_DATA_DIR=str(DATA_DIR))  # noqa: F821


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

    # Mirror-sync: data dirs are gitignored and persist across checkouts, so a
    # deleted or renamed source file would otherwise keep shipping its stale
    # .gz in every locally built filesystem image, forever. The data dir is
    # per-env, so anything not expected for THIS env is stale by definition.
    for old in DST.rglob("*.gz"):
        if old not in expected:
            old.unlink()
            print(f"pruned stale {old.relative_to(PROJECT)}")


pack()
