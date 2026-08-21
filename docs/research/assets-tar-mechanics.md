# Tar mechanics for incremental asset updates (wayfinder #123)

Research for the staged-tar asset update design (#122): ustar parsing on-device, parser choice, LittleFS directory rename viability, cheap validation, and deterministic tar in CI. Investigated 2026-08-21 against the installed framework (arduino-esp32 3.3.11 / ESP-IDF 5.5.5, `versions.txt`: `joltwallet__littlefs: 1.22.2`, vendoring littlefs upstream commit `6cb4e86` = LFS_VERSION 2.11) and primary sources cited inline.

## Verdict

- **Parser: hand-roll** a ~100-line read-only ustar walker. rxi/microtar is MIT and tiny (376+90 lines) but its read path requires a `seek` callback (it re-seeks to `last_header` on every `mtar_read_header`), which fights a forward-only HTTP stream, its last code commit is 2017, and it has a pile of open stack-buffer-overflow reports in exactly the header-parsing paths we would rely on (`strcpy` on non-NUL-terminated `name`, unbounded `sscanf("%o")`). Our case (flat ASCII names ~20 chars, regular files only, trusted CI-produced archive) is smaller than microtar's surface area.
- **Dir rename works.** `LittleFS.rename("/w", "/w.v0.3.2")` is viable: the Arduino wrapper is path-only (no file/dir distinction), esp_littlefs forwards straight to `lfs_rename`, and littlefs's `lfs_rename` explicitly supports directories and commits the move as a single atomic metadata commit (power-loss safe via gstate orphan/move tracking). One caveat: close every open file under `/w` first — esp_littlefs only EBUSY-guards exact-path matches, not descendants.
- **Validation: GitHub's per-asset sha256 `digest` + streamed hash.** Header-checksum walking alone does NOT protect file content (the chksum covers only the 512-byte header). GitHub release assets have exposed a `digest` field (sha256) in the REST API since June 2025; the device already talks to that API for ghupdate. Stream the tar through mbedTLS SHA-256 while unpacking into the staging dir, compare against the asset's `digest` at the end, roll back the staging dir on mismatch. No sidecar asset or embedded-digest-entry needed.
- **CI tar invocation** on ubuntu-latest (Ubuntu 24.04, GNU tar 1.35): `tar --format=ustar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf assets.tar -C <dir> .` — all flags exist (`--sort=name` needs tar ≥ 1.28). `--format=ustar` keeps the device parser trivial: no pax `x`/`g` or GNU `L`/`K` entries can appear for our flat short names.

## 1. ustar header layout a minimal streaming unpacker must honor

Source: POSIX.1-2017 `pax` utility, "ustar Interchange Format" — https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html

Field layout (offset/length in bytes within each 512-byte header block):

| field | offset | len | | field | offset | len |
|---|---|---|---|---|---|---|
| name | 0 | 100 | | linkname | 157 | 100 |
| mode | 100 | 8 | | magic | 257 | 6 |
| uid | 108 | 8 | | version | 263 | 2 |
| gid | 116 | 8 | | uname | 265 | 32 |
| size | 124 | 12 | | gname | 297 | 32 |
| mtime | 136 | 12 | | devmajor | 329 | 8 |
| chksum | 148 | 8 | | devminor | 337 | 8 |
| typeflag | 156 | 1 | | prefix | 345 | 155 |

Rules the walker must implement, all from the POSIX page above:

- **Octal fields**: numeric fields are "leading zero-filled octal numbers … terminated by one or more `<space>` or NUL characters". Parse `size` as octal, stopping at space/NUL; do not assume NUL-only termination (GNU tar historically space-terminates some fields).
- **Checksum**: `chksum` is the octal representation of the simple unsigned sum of all 512 header bytes, computed with the chksum field itself treated as 8 ASCII spaces (0x20). Verify it on every header. (Defensive extra from common practice: some ancient tars wrote a signed-byte sum; irrelevant for GNU-tar-produced archives, skip the fallback.)
- **typeflag**: regular files are `'0'`; a NUL typeflag "should be recognized as meaning a regular file when extracting". Accept `'0'` and `'\0'`, treat `'5'` (directory) as skip-or-mkdir if we ever ship one, and **reject the archive** on anything else — in particular pax extended headers `'x'`/`'g'` and GNU long-name/long-link `'L'`/`'K'`, which cannot appear in `--format=ustar` output for 20-char flat names but whose presence would mean CI produced the wrong format.
- **Content padding**: file data occupies `(size + 511) / 512` blocks; skip the padding after each file's content.
- **End of archive**: "two 512-octet logical records filled with binary zeros". Detect the first all-zero block, require the second, then stop. (Practical shortcut used by microtar too: a header whose chksum field starts with NUL is treated as the zero terminator.)
- **magic/version**: POSIX ustar writes `magic = "ustar\0"`, `version = "00"`. Old GNU format instead writes `"ustar  \0"` (magic+version = `ustar`, two spaces, NUL) — GNU tar 1.35's default output format is still `gnu` unless `--format` is given ("Usually, GNU tar is configured to create archives in 'gnu' format", https://getdocs.org/Tar/docs/latest/Formats — same text as the GNU tar 1.35 manual, Formats section). Since we pin `--format=ustar` in CI, check `memcmp(magic, "ustar", 5) == 0` and ignore the version bytes; that also tolerates a stray gnu-format archive of short flat names, which is byte-identical apart from magic/version.
- **prefix**: for our flat ≤20-char names the `prefix` field is all NUL; the walker can ignore it (name fits entirely in `name[100]`).

**GNU tar vs bsdtar for a flat archive of 6 small files**: GNU tar with `--format=ustar` emits pure ustar, one header block per file, no extension entries. bsdtar's default write format is "restricted pax", which "will be identical to a ustar archive unless the extended attributes entry is required to store a long file name, long linkname, extended ACL, file flags, or if any of the standard ustar data … cannot be fully represented in the ustar header" (libarchive `libarchive-formats.5`, lines 111–123, https://github.com/libarchive/libarchive/blob/master/libarchive/libarchive-formats.5) — in practice bsdtar can still emit a pax `x` entry per file for sub-second mtimes, so CI must use GNU tar with an explicit `--format=ustar`, and the device parser rejects `x`/`g`/`L`/`K` typeflags outright rather than skipping them silently.

## 2. Parser: vendor rxi/microtar vs hand-roll

Source inspected: https://github.com/rxi/microtar at master (`src/microtar.c` 376 lines, `src/microtar.h` 90 lines; MIT; 504 stars; repo metadata via GitHub API).

- **API shape**: callback-based (`tar->read`, `tar->seek`, `tar->close`, `tar->write`), so it can be pointed at a network stream — but the **read path requires a working `seek`**: `mtar_read_header()` first does `mtar_seek(tar, tar->last_header)` (microtar.c:270–281) and `mtar_next()` seeks forward past data (microtar.c:234–240). Backing an HTTP stream means faking seek with a position-tracking shim that can only seek forward-to-current, which is more glue than the parser saves.
- **Maintenance**: last code commit 2017-08-31 (GitHub API, latest commit on master); 27 open issues, none triaged.
- **Known issues in exactly our code path**: open reports #28/#29/#30 — stack buffer overflow in `raw_to_header()` via `strcpy` on non-NUL-terminated `name`/`linkname` fields (microtar.c:111–112 copies a 100-byte field that POSIX says may legally be non-NUL-terminated when exactly 100 chars); unbounded `sscanf(rh->size, "%o", …)` (microtar.c:106–109); #24 empty archive unreadable. Write-path overflows #31/#32 don't affect a read-only user but confirm the hygiene level.
- **Espressif component registry**: no tar component from Espressif; the registry carries third-party wrappers of similarly small parsers, nothing better-maintained than microtar for this job.

**Verdict**: hand-roll. A read-only, forward-only ustar walker for trusted flat archives is ~100 lines: read 512, detect zero-block terminator, verify chksum, parse octal size with bounded loop, bounds-check name (NUL-pad copy into a 101-byte buffer), switch on typeflag, stream `size` bytes to LittleFS, skip padding. Every microtar CVE-shaped bug above is a consequence of generality we don't need, and the sha256 digest check (§4) already gates content integrity before any parsed value is trusted much.

## 3. LittleFS directory rename through the Arduino wrapper

Three layers, all read:

1. **Arduino wrapper** — `C:\Users\petere\.platformio\packages\framework-arduinoespressif32\libraries\FS\src\vfs_api.cpp:101-136` (`VFSImpl::rename`): validates both paths start with `/`, prepends the mount point (default base path `/littlefs`, `LittleFS.h:29`; our `ConfigStore.cpp:285` uses exactly that), and calls libc `::rename(temp1, temp2)`. **No file-vs-directory distinction at this layer**; libc rename dispatches through the ESP-IDF VFS to the registered handler.
2. **esp_littlefs VFS layer** — joltwallet/esp_littlefs v1.22.2 (the version named in `framework-arduinoespressif32-libs/versions.txt`; shipped precompiled as `libjoltwallet__littlefs.a`), `src/esp_littlefs.c:2404-2445` (`vfs_littlefs_rename`, registered as `.rename_p` at :417): takes the fs semaphore, returns EBUSY if **the exact src or dst path** is an open file (`esp_littlefs_get_fd_by_name`), then calls `lfs_rename(efs->fs, src, dst)` directly — src may be a file or a directory, nothing checks. (`CONFIG_LITTLEFS_SPIFFS_COMPAT` is off in the shipped sdkconfig — `framework-arduinoespressif32-libs/esp32/sdkconfig:3289` — so no mkdirs/rmdirs side effects.) https://github.com/joltwallet/esp_littlefs/blob/v1.22.2/src/esp_littlefs.c
3. **littlefs core** — upstream commit `6cb4e86` (v2.11, the submodule pin of esp_littlefs v1.22.2), `lfs.h:512-520`: "Rename or move a file or directory. If the destination exists, it must match the source in type. If the destination is a directory, the directory must be empty." Implementation `lfs.c:3969-4090` (`lfs_rename_`): explicitly handles `LFS_TYPE_DIR` sources/destinations and lands the entire move as **one `lfs_dir_commit`** carrying CREATE + name + `LFS_FROM_MOVE` + DELETE attributes, with gstate move/orphan tracking so an interrupted rename is repaired by `lfs_fs_forceconsistency` on the next mount — i.e. the rename is atomic under power loss (the file either has the old name or the new name, never neither). https://github.com/littlefs-project/littlefs/blob/6cb4e86540eca0d9ba62500a298385c9d863c8be/lfs.c

**So `LittleFS.rename("/w", "/w.v0.3.2")` works and is atomic.** Caveats found while reading:

- **Close files first**: the EBUSY guard at esp_littlefs.c:2410-2421 matches the exact path only. A file open as `/w/index.html` does not block renaming `/w`, and its cached fd/path state goes stale. The updater must ensure nothing under `/w` is open (our web server serves from it) before the swap.
- **Path shape**: give the wrapper `/w` and `/w.v0.3.2` (leading slash required, vfs_api.cpp:108; no trailing slashes — littlefs tolerates a trailing slash on dir paths (`lfs_path_isdir` handling in `lfs_rename_`), but there's no reason to poke that).
- **Name length**: dst name must fit `lfs->name_max`, set from `CONFIG_LITTLEFS_OBJ_NAME_LEN=64` (sdkconfig:3278) — `/w.v0.3.2`-style names are nowhere close.
- **Destination must not exist** (or be an empty dir of the same type) — `lfs.c:4017-4021` returns ISDIR/NOTDIR on type mismatch, NOTEMPTY on a non-empty dir. Delete any leftover `/w.<ver>` from a previous failed run before renaming.
- **No per-file fallback needed.** (If it had been broken: 6 files × `rename()` ≈ 6 metadata commits, still cheap, but it loses the single-atomic-swap property — moot.)

## 4. Cheap full-archive validation

**Walking all headers + verifying each chksum to the two-zero-block terminator is NOT sufficient for content integrity.** The chksum covers only the 512 header bytes (POSIX, §1 above); file content blocks are entirely unprotected by the format. What the walk *does* reject: truncation (stream ends before the declared sizes + terminator are consumed), header corruption, and format surprises. Random corruption inside a file's data blocks sails through. In practice TLS already gives in-transit integrity, so the residual risks are truncated downloads (walk catches), a wrong/stale asset, and flash-side write errors — the last two need a content digest.

**GitHub gives us the digest for free.** Since 2025-06-03, "releases now expose digests for release assets": every uploaded asset gets an immutable sha256, surfaced as the `digest` property on the release-asset object in the REST API (e.g. `"digest": "sha256:2151b6…"`), visible in `GET /repos/{owner}/{repo}/releases/latest` — the exact call ghupdate already makes for firmware. Changelog: https://github.blog/changelog/2025-06-03-releases-now-expose-digests-for-release-assets/ ; schema: https://docs.github.com/en/rest/releases/assets (release asset object). Only pre-June-2025 assets lack it, which cannot affect us.

**Recommended scheme (simplest sound)**: when checking the release, read `digest` off the asset object alongside `browser_download_url`. Stream the tar download through an incremental mbedTLS SHA-256 context *while* the ustar walker unpacks into the staging area; at end-of-stream compare the computed hash to `digest`. On mismatch (or walk failure) delete the staging dir and abort before any swap — the `/w` rename in #122's design only happens after both checks pass, so a bad download can never touch the live tree. This beats a sha256 sidecar asset (second download, pairing logic) and beats embedding a digest as the tar's first entry (CI must hash-then-prepend, and the digest can't cover itself — GitHub's asset digest covers the whole tar including that entry, making it redundant).

## 5. Deterministic tar in CI

ubuntu-latest is Ubuntu 24.04 (https://github.com/actions/runner-images README), which ships GNU tar **1.35** (`tar 1.35+dfsg-3ubuntu0.4`, https://packages.ubuntu.com/noble/tar). The reproducible-builds.org canonical recipe (https://reproducible-builds.org/docs/archives/) is `tar --sort=name --mtime="@${SOURCE_DATE_EPOCH}" --owner=0 --group=0 --numeric-owner …` and notes `--sort=name` requires GNU tar ≥ 1.28 (`--clamp-mtime` ≥ 1.29, not needed here) — all comfortably present in 1.35. Their `--pax-option` flag is only needed for posix-format archives; we use ustar, which has no per-entry extended headers to scrub.

Exact invocation for the asset archive:

```sh
tar --format=ustar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -cf assets-<env>.tar -C <staging-dir> .
```

Notes: `--format=ustar` guarantees no pax/GNU extension entries (§1) and pins `magic="ustar\0" version="00"`; `--sort=name` fixes entry order independent of filesystem readdir order; `--mtime=@0` + owner/group/numeric-owner zero the remaining varying header fields, so identical inputs produce identical bytes — and therefore an identical GitHub asset `digest`, which makes #122's "idempotent same-tag reinstall" check trivially stable. If entries should be named `w/foo` rather than `./w/foo`, build the staging dir accordingly and list members explicitly instead of `.` (still after `--sort=name`); avoid `--transform` since it runs before sorting keys are compared in some tar versions — simpler to lay out the staging dir correctly.
