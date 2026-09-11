# Put $IDF_PATH's esp_bignum.c into one known state, or report which state it is in.
#
# The spike measures patches that live in the SDK TREE, not in this repo, so the
# thing being measured is not visible in any file here. That has already produced
# one silently wrong result: a patched cell was built, the patch was reverted, and
# `idf.py flash` recompiled it unpatched during the flash step. The numbers came
# back byte-identical to the unpatched run, which is the only reason it was caught.
#
# So: never switch the tree by hand. run.ps1 calls this immediately before the
# flash (which is the moment that matters, because flashing rebuilds), and asserts
# the result. Use -Verify to ask what is in the tree right now.
#
#   ./sdk_variant.ps1 -Verify
#   ./sdk_variant.ps1 -Set fix
#   ./sdk_variant.ps1 -Set fix-stackcopy
#   ./sdk_variant.ps1 -Set fix-stackcopy-wipe
#   ./sdk_variant.ps1 -Set stock          # restore the pristine SDK file
#
# Variants:
#   stock          v6.0.2 as shipped: every multiply goes to the RSA peripheral.
#   fix            + upstream/esp-idf-mpi-min-bitlen.patch (espressif/esp-idf#19027):
#                  sub-threshold multiplies route to software, which copies an
#                  aliased operand into a HEAP mbedtls_mpi.
#   fix-stackcopy  + upstream/esp-idf-mpi-stack-copy.patch, the change asked for in
#                  the #19027 review on 2026-09-04: the same copy onto the STACK,
#                  bounded by the routing threshold.
#
#   fix-stackcopy-wipe
#                  + upstream/esp-idf-mpi-zeroize-copy.patch, which wipes those
#                  stack copies. Pushed 2026-09-08, reverted 2026-09-09 on a
#                  cross-build measurement that was wrong, reapplied 2026-09-10
#                  after measuring it within one binary at 0.22-1.05%. This is
#                  the state the firmware ships.
#
# `fix-stackcopy-wipe` is the state the real firmware expects: cmake/upstream_patches.cmake
# applies all three patches as a series, and FATALs if that series neither
# applies nor reverse-applies. So leaving the tree in any other state makes the
# next smolbase build fail loudly rather than build a hybrid. That is by design,
# and it is why this script exists: run `-Set fix-stackcopy-wipe` when you are done here.

[CmdletBinding(DefaultParameterSetName = 'Verify')]
param(
    [Parameter(ParameterSetName = 'Set', Mandatory)]
    [ValidateSet('stock', 'fix', 'fix-stackcopy', 'fix-stackcopy-wipe', 'fix-stackcopy-count', 'fix-stackcopy-ab', 'fix-heapab')]
    [string]$Set,

    # The TF-PSA-Crypto #873 constant-time series, in the mbedtls SUBMODULE --
    # a different repo from the one $Set patches, and left alone by it. It makes
    # the software bignum path faster, which moves the hardware/software
    # crossover UP, so a crossover measured with it in is not comparable with one
    # measured without. 'in' is what the firmware ships and the default here;
    # 'out' exists to measure exactly that difference.
    [Parameter(ParameterSetName = 'Set')]
    [ValidateSet('in', 'out')]
    [string]$Ct = 'in',

    [Parameter(ParameterSetName = 'Verify')]
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$idf = $env:IDF_PATH
if (-not $idf) { throw "IDF_PATH is not set. Source the SDK first: & `$HOME\esp\esp-idf\export.ps1" }

$rel = 'components/mbedtls/port/bignum/esp_bignum.c'
$abs = Join-Path $idf $rel
if (-not (Test-Path $abs)) { throw "no $rel under IDF_PATH ($idf)" }

$thresholdPatch = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-min-bitlen.patch'
$stackCopyPatch = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-stack-copy.patch'
$zeroizePatch   = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-zeroize-copy.patch'
# Self-contained (min-bitlen + stack copy + an entry counter), applied to a
# pristine file rather than stacked, because it is a diagnostic and not a
# candidate for upstream.
$softCountPatch = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-softcount.patch'
$wipeAbPatch    = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-wipeab.patch'
$heapAbPatch    = Join-Path $PSScriptRoot 'upstream/esp-idf-mpi-heapab.patch'

# The #873 series lives in the repo's own patches/ directory, because the real
# firmware build applies it from there via cmake/upstream_patches.cmake. Same
# files, one source of truth.
$mbedRepo = Join-Path $idf 'components/mbedtls/mbedtls'
$ctPatches = @(
    '0001-constant_time-force-inlining-where-an-assembly-path-.patch',
    '0002-constant_time-add-an-Xtensa-assembly-path.patch',
    '0003-bignum_core-use-_if_else_0-where-the-zero-branch-is-.patch'
) | ForEach-Object { Join-Path $PSScriptRoot "../../patches/$_" }

function Get-CtState {
    # Read the tree, not a flag. Clean submodule = series out.
    $dirty = git -C $mbedRepo status --porcelain
    if ($dirty) { return 'in' }
    return 'out'
}

# Read the variant out of the SOURCE, never out of a tag or a build directory
# name. Same principle as the spike's [id] line reading hw_mul from the compiled
# macro rather than from the argfile.
function Get-SdkVariant {
    $src = Get-Content $abs -Raw
    # Most specific first: the wipe implies the stack copy.
    if ($src -match 'esp_mpi_soft_heapcopy') { return 'fix-heapab' }
    if ($src -match 'esp_mpi_soft_wipe') { return 'fix-stackcopy-ab' }
    if ($src -match 'esp_mpi_soft_call_count') { return 'fix-stackcopy-count' }
    if ($src -match 'mbedtls_platform_zeroize\(tx') { return 'fix-stackcopy-wipe' }
    if ($src -match 'MPI_SOFT_MAX_WORDS') { return 'fix-stackcopy' }
    if ($src -match 'mpi_mult_mpi_soft') { return 'fix' }
    return 'stock'
}

if ($PSCmdlet.ParameterSetName -eq 'Verify') {
    $v = Get-SdkVariant
    Write-Host "$rel is: $v   (#873 constant-time series: $(Get-CtState))"
    return $v
}

# Rebuild the state from pristine every time rather than stepping between
# variants. Reverse-applying a series is where state machines go wrong, and this
# one file has no local changes worth keeping -- every modification it can carry
# is regenerable from upstream/*.patch.
git -C $idf checkout -- $rel
if ($LASTEXITCODE -ne 0) { throw "could not restore $rel in the SDK tree" }

$applied = @()
if ($Set -eq 'fix-heapab') {
    git -C $idf apply -p1 --include=$rel $heapAbPatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $heapAbPatch" }
    $applied += 'esp-idf-mpi-heapab.patch (combined)'
}
elseif ($Set -eq 'fix-stackcopy-ab') {
    git -C $idf apply -p1 --include=$rel $wipeAbPatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $wipeAbPatch" }
    $applied += 'esp-idf-mpi-wipeab.patch (combined)'
}
elseif ($Set -eq 'fix-stackcopy-count') {
    git -C $idf apply -p1 --include=$rel $softCountPatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $softCountPatch" }
    $applied += 'esp-idf-mpi-softcount.patch (combined)'
}
elseif ($Set -ne 'stock') {
    # --include: the patch file also carries a CMakeLists hunk that the PR branch
    # does not have, exactly as cmake/upstream_patches.cmake does it.
    git -C $idf apply -p1 --include=$rel $thresholdPatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $thresholdPatch" }
    $applied += 'esp-idf-mpi-min-bitlen.patch'
}
if ($Set -eq 'fix-stackcopy' -or $Set -eq 'fix-stackcopy-wipe') {
    git -C $idf apply -p1 --include=$rel $stackCopyPatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $stackCopyPatch" }
    $applied += 'esp-idf-mpi-stack-copy.patch'
}
# The Cursor Bugbot finding on #19027: the stack copies were not wiped, while
# the heap MPIs they replaced were wiped by mbedtls_mpi_free(). This variant
# exists to price the wipe against the win the stack copy buys.
if ($Set -eq 'fix-stackcopy-wipe') {
    git -C $idf apply -p1 --include=$rel $zeroizePatch
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $zeroizePatch" }
    $applied += 'esp-idf-mpi-zeroize-copy.patch'
}

$got = Get-SdkVariant
if ($got -ne $Set) { throw "asked for '$Set' but the tree now reads as '$got' -- the patches did not produce what they claim" }

# The #873 series, in the submodule. Same rebuild-from-pristine approach: reset
# the submodule, then re-apply if asked. `git checkout -- .` inside the submodule
# rather than `git submodule update`, which would also move HEAD.
if ((Get-CtState) -ne $Ct) {
    git -C $mbedRepo checkout -- .
    if ($LASTEXITCODE -ne 0) { throw "could not restore the mbedtls submodule" }
    if ($Ct -eq 'in') {
        foreach ($p in $ctPatches) {
            git -C $mbedRepo apply --directory=tf-psa-crypto -p1 $p
            if ($LASTEXITCODE -ne 0) { throw "failed to apply $p -- the submodule may now be partially patched; git -C $mbedRepo checkout -- . and retry" }
        }
    }
    $now = Get-CtState
    if ($now -ne $Ct) { throw "asked for #873 '$Ct' but the submodule reads as '$now'" }
}

$what = if ($applied) { $applied -join ' + ' } else { 'pristine v6.0.2' }
Write-Host "SDK $rel -> $Set ($what);  #873 series: $(Get-CtState)"
return $got
