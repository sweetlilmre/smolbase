// Incremental web-asset updates: staged ustar tar with a version-named backup
// of /w (wayfinder map #121, design #122, mechanics research #123).
//
// Update path (firmware changing): the GhUpdate task downloads the release's
// <app>-assets-<tag>.tar to a staging file (SHA-256-verified against the
// GitHub asset digest), renames /w -> /w.<running-version>, extracts, deletes
// the tar, and only then finalizes the firmware. Rollback or a torn update is
// healed by bootHeal(): a backup named for the *running* version is the
// running version's assets — restore it. The backup for a *different* version
// is deleted once the new image survives the 30 s rollback guard
// (onImageConfirmed()).
#pragma once
#include <stddef.h>

namespace AssetUpdate {

// Progress callback: filesDone/filesTotal while extracting.
using FileProgressCb = void (*)(int done, int total);

// Fetch the sha256 digest GitHub records for <assetsPrefix>-<tag>.tar in the
// release tagged `tag`. outHex must hold 65 bytes. False if the release or
// asset is missing, or the asset has no digest.
bool fetchAssetDigest(const char* tag, char* outHex, size_t outHexLen, char* errBuf, size_t errLen);

// Download the release tar to the staging file, streaming through SHA-256,
// and compare with expectedHex. False (with staging file removed) on any
// mismatch or transport error.
bool downloadTar(const char* tag, const char* expectedHex, char* errBuf, size_t errLen);

// Walk the staged tar: verify header checksums, typeflags, sizes, and the
// zero-block terminator. Returns the number of file entries, or -1.
int validateTar(char* errBuf, size_t errLen);

// Firmware-change path: rename /w -> /w.<running-version>, extract the staged
// tar into a fresh /w, delete the tar. On failure restores /w from the backup
// before returning false. Call only after validateTar() succeeded.
bool applyTarWithBackup(FileProgressCb cb, char* errBuf, size_t errLen);

// Same-version reinstall path: extract over the live /w via per-file
// tmp+rename, then delete /w files the tar does not carry. No backup, no
// reboot needed. Deletes the staged tar when done.
bool applyTarInPlace(FileProgressCb cb, char* errBuf, size_t errLen);

// Delete any stale staging tar or /w.<ver> backup left by an interrupted
// update. Call at update start.
void sweepStaleStaging();

// Call once at boot, after LittleFS is mounted and before the web server
// starts: if /w.<ver> matches the running version we were rolled back (or the
// update was torn) — restore it. Also removes a stale staging tar.
void bootHeal();

// Call when the rollback guard confirms the running image healthy: deletes a
// /w.<ver> backup belonging to a different (older) version, and any stale tar.
void onImageConfirmed();

} // namespace AssetUpdate
