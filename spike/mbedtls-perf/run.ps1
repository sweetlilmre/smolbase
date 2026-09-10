# Driver for the mbedTLS perf spike: flash one configuration, capture its
# results over the UART, and collate every captured run into one CSV.
#
# FLASHING THIS SPIKE ERASES THE SMOLBASE FIRMWARE. It writes its own bootloader
# and its own (default, single_app) partition table, so the device's custom
# layout from partitions.csv goes with it, along with the NVS holding the WiFi
# credentials. Restoring the device afterwards is a full serial flash of the real
# firmware and a re-provision through the captive portal. Nothing here flashes
# without -Flash, and -Flash is not the default for that reason.
#
#   ./run.ps1 -ParseOnly                       # collate whatever is captured
#   ./run.ps1 -Runs idf6-mpi-on -Flash         # flash, capture, collate one run
#   ./run.ps1 -Flash                           # all eight, one at a time
#
# Each run needs its SDK active, because the flash step calls idf.py:
#   & $HOME\esp\esp-idf\export.ps1        for the idf6-* runs
#   & $HOME\esp\esp-idf-v5.5\export.ps1   for the idf5-* runs
# The script refuses a run whose tag does not match the active IDF_PATH rather
# than flashing an image built by the other SDK.

[CmdletBinding()]
param(
    # Left unset, the port is chosen from the tag: the ESP32-S3 cells run on the
    # second board, the rest on the bench device. Pass -Port to override.
    [string]$Port,
    [string[]]$Runs = @(
        'idf5-mpi-on', 'idf5-mpi-off', 'idf5-mpi-on-fp', 'idf5-mpi-off-fp',
        'idf6-mpi-on', 'idf6-mpi-off', 'idf6-mpi-on-fp', 'idf6-mpi-off-fp',
        # The uninstrumented control. The hook costs 2.33 us per call with the
        # accelerator off and 11.08 us with it on, and an ECDSA verify makes
        # thousands of hooked calls -- so the accelerator on-vs-off comparison at
        # the verify level is only trustworthy from these four.
        'idf5-mpi-on-nowrap', 'idf5-mpi-off-nowrap',
        'idf6-mpi-on-nowrap', 'idf6-mpi-off-nowrap'
    ),
    [string]$OutDir = 'results',
    [int]$TimeoutSec = 300,
    [switch]$Flash,
    [switch]$ParseOnly,
    # The SDK-tree patch state is synced from the tag before every flash. Opt out
    # only if you are deliberately driving sdk_variant.ps1 yourself.
    [switch]$NoSdkSync,
    # The MAC check refuses to flash a board that is not the one the tag expects.
    [switch]$SkipMacCheck
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Which board a tag belongs on, and how to know it is that board. The second
# ESP32-S3 shares a bench with a Waveshare "xiaozhi" board that is NOT ours;
# flashing that one would be somebody else's bad day, so the MAC is checked
# read-only before anything is written.
$BOARDS = @{
    esp32   = @{ Port = 'COM5'; Mac = 'b4:bf:e9:c8:2e:00' }
    esp32s3 = @{ Port = 'COM9'; Mac = '28:84:85:8d:31:b4' }
}

function Get-BoardKey { param([string]$Tag) if ($Tag -match '-s3-') { 'esp32s3' } else { 'esp32' } }

# Which SDK-tree state a tag is measuring. Read the tag, set the tree, then read
# the tree back -- see the header of sdk_variant.ps1 for why this is not optional.
function Get-WantedSdkVariant {
    param([string]$Tag)
    if ($Tag -match '-fix-stackcopy-ab') { return 'fix-stackcopy-ab' }
    if ($Tag -match '-fix-stackcopy-count') { return 'fix-stackcopy-count' }
    if ($Tag -match '-fix-stackcopy-wipe') { return 'fix-stackcopy-wipe' }
    if ($Tag -match '-fix-stackcopy') { return 'fix-stackcopy' }
    if ($Tag -match '-fix') { return 'fix' }
    return 'stock'
}

# The TF-PSA-Crypto #873 series lives in a different repo (the mbedtls submodule)
# and is IN for every cell except the '-ct-out' controls, which exist to price
# what it does to the crossover.
function Get-WantedCt { param([string]$Tag) if ($Tag -match '-ct-out$') { 'out' } else { 'in' } }

function Assert-Board {
    param([string]$Port, [string]$ExpectMac, [string]$Tag)
    $out = & python -m esptool --port $Port --no-stub chip-id 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "${tag}: could not read the chip id on $Port before flashing. Is the board attached and the port free?`n$out"
    }
    if ($out -notmatch [regex]::Escape($ExpectMac)) {
        throw "${tag}: $Port is NOT the expected board (MAC $ExpectMac). Refusing to flash. esptool said:`n$out"
    }
    Write-Host "$tag : $Port confirmed as $ExpectMac"
}

# ---- Serial capture -----------------------------------------------------
# A default System.IO.Ports.SerialPort drops bytes during the boot burst and
# produces plausible-but-wrong text -- mangled IP addresses, half-eaten log
# lines. The fix, verified both ways during the migration, is a large receive
# buffer plus a tight ReadExisting() loop that does NOTHING else. Nothing may be
# added inside this loop: no curl, no Write-Progress, no file I/O.
function Capture-Boot {
    param([string]$Port, [int]$TimeoutSec, [string]$Until)

    $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'one'
    $sp.ReadBufferSize = 524288
    $sp.ReadTimeout = 50
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Open()
    try {
        # Reset via RTS with DTR held low: the adapter on this bench is wired for
        # auto-reset, so this is a reboot without a reflash and without touching
        # GPIO0.
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds 150
        $sp.RtsEnable = $false

        $sb = New-Object System.Text.StringBuilder
        $tail = ''
        $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSec)
        while ([datetime]::UtcNow -lt $deadline) {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) {
                [void]$sb.Append($chunk)
                # Only the last 64 characters are searched, so the marker test
                # stays O(1) as the transcript grows and cannot become the thing
                # that makes the loop too slow to keep up.
                $tail = ($tail + $chunk)
                if ($tail.Length -gt 64) { $tail = $tail.Substring($tail.Length - 64) }
                if ($tail.Contains($Until)) { break }
            }
        }
        return $sb.ToString()
    }
    finally {
        $sp.Close()
        $sp.Dispose()
    }
}

# ---- Parsing ------------------------------------------------------------
# The device prints one key=value line per benchmark. Parsing them here rather
# than reading them off the screen is what makes eight runs comparable without
# transcription errors.
function Parse-Run {
    param([string]$Tag, [string]$Text)

    $rows = @()
    $id = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match '^\[id\]\s+(.*)$') {
            foreach ($kv in ($Matches[1] -split '\s+')) {
                $p = $kv -split '=', 2
                if ($p.Count -eq 2) { $id[$p[0]] = $p[1] }
            }
        }
        elseif ($line -match '^\[bench\]\s+(.*)$') {
            $f = @{}
            foreach ($kv in ($Matches[1] -split '\s+')) {
                $p = $kv -split '=', 2
                if ($p.Count -eq 2) { $f[$p[0]] = $p[1] }
            }
            $rows += [pscustomobject][ordered]@{
                run              = $Tag
                mbedtls          = $id['mbedtls']
                # Read from the capture, not from the tag. Captures taken before
                # these fields existed have none, and '' is the honest answer
                # there -- inferring them from the filename is how a table ends
                # up asserting something nobody measured.
                target           = if ($id.ContainsKey('target')) { $id['target'] } else { '' }
                cpu_mhz          = if ($id.ContainsKey('cpu_mhz')) { $id['cpu_mhz'] } else { '' }
                cpu_mhz_rt       = if ($id.ContainsKey('cpu_mhz_rt')) { $id['cpu_mhz_rt'] } else { '' }
                mpi_min_bits     = if ($id.ContainsKey('mpi_min_bits')) { $id['mpi_min_bits'] } else { '' }
                hw_mul           = $id['hw_mul']
                fixed_point      = $id['fixed_point']
                # Captures taken before the no-wrap control existed have no
                # wrap= field in their [id] line; every one of them was
                # instrumented, so absent means 1.
                wrap             = if ($id.ContainsKey('wrap')) { $id['wrap'] } else { '1' }
                bench            = $f['name']
                bits             = [int]$f['bits']
                iters            = [int]$f['iters']
                min_us           = [math]::Round([double]$f['min_ns'] / 1000, 3)
                mean_us          = [math]::Round([double]$f['mean_ns'] / 1000, 3)
                max_us           = [math]::Round([double]$f['max_ns'] / 1000, 3)
                mul_calls_per_op = [math]::Round([double]$f['mul_calls_per_op'] / 1000, 3)
                mul_us_per_op    = [math]::Round([double]$f['mul_ns_per_op'] / 1000, 3)
                exp_calls_per_op = [math]::Round([double]$f['exp_calls_per_op'] / 1000, 3)
                exp_us_per_op    = [math]::Round([double]$f['exp_ns_per_op'] / 1000, 3)
                soft_calls_per_op = if ($f.ContainsKey('soft_calls_per_op')) { [math]::Round([double]$f['soft_calls_per_op'] / 1000, 3) } else { '' }
                rc               = $f['rc']
            }
        }
    }
    return $rows
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $ParseOnly) {
    foreach ($tag in $Runs) {
        $buildDir = Join-Path 'build' $tag
        if (-not (Test-Path (Join-Path $buildDir 'mbedtls_perf.bin'))) {
            throw "$tag has not been built: $buildDir\mbedtls_perf.bin is missing. Activate that SDK and run: idf.py '@$tag.args' build"
        }

        # Refuse to flash an image built by the other SDK. The tag says which
        # mbedTLS this run is measuring, and a mismatch here would silently
        # produce a whole table of results attributed to the wrong version.
        $wantIdf = if ($tag.StartsWith('idf5')) { 'esp-idf-v5.5' } else { 'esp-idf' }
        $haveIdf = Split-Path -Leaf $env:IDF_PATH
        if ($haveIdf -ne $wantIdf) {
            throw "$tag needs IDF_PATH ending in '$wantIdf' but the active SDK is '$haveIdf'. Source the right export.ps1 and re-run with -Runs $tag."
        }

        # Assert the configuration landed in the GENERATED sdkconfig. A Kconfig
        # value that failed to take reads as a perfectly plausible measurement
        # of the wrong thing, and that has already happened once in this project.
        $cfg = Get-Content (Join-Path $buildDir 'sdkconfig')
        $wantMpi = -not $tag.Contains('mpi-off')
        $wantFp = $tag.EndsWith('-fp') -or $tag.Contains('-fp-')
        $haveMpi = [bool]($cfg -match '^CONFIG_MBEDTLS_HARDWARE_MPI=y$')
        $haveFp = [bool]($cfg -match '^CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM=y$')
        if ($haveMpi -ne $wantMpi) { throw "${tag}: HARDWARE_MPI is $haveMpi in the generated sdkconfig, expected $wantMpi" }
        if ($haveFp -ne $wantFp) { throw "${tag}: ECP_FIXED_POINT_OPTIM is $haveFp in the generated sdkconfig, expected $wantFp" }

        # The chip, from the generated sdkconfig rather than from the argfile. An
        # s3 tag built for esp32 would otherwise flash, run, and produce a whole
        # table of plausible numbers attributed to the wrong silicon.
        $wantTarget = if ($tag -match '-s3-') { 'esp32s3' } else { 'esp32' }
        $haveTarget = ($cfg | Select-String -Pattern '^CONFIG_IDF_TARGET="(.+)"$').Matches.Groups[1].Value
        if ($haveTarget -ne $wantTarget) { throw "${tag}: built for '$haveTarget' but the tag says '$wantTarget'" }

        # The CPU frequency, for the crossover-vs-frequency cells. It is a Kconfig
        # CHOICE, which needs the current pick explicitly unset before the
        # alternative takes -- so "the fragment is in SDKCONFIG_DEFAULTS" is not
        # evidence that the frequency changed. Assert the generated value.
        if ($tag -match '-(?:xover|mulstep)-(\d+)(?:-ct-out)?$') {
            $wantMhz = [int]$Matches[1]
            $haveMhz = [int]($cfg | Select-String -Pattern '^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=(\d+)$').Matches.Groups[1].Value
            if ($haveMhz -ne $wantMhz) { throw "${tag}: generated sdkconfig says ${haveMhz} MHz, the tag says ${wantMhz} MHz" }
        }

        if (-not $Flash) {
            Write-Host "$tag : built and config-checked. Re-run with -Flash to flash and capture (this ERASES the smolbase firmware)."
            continue
        }

        $board = $BOARDS[(Get-BoardKey $tag)]
        $usePort = if ($Port) { $Port } else { $board.Port }

        # The SDK tree, immediately before the flash. This is the moment that
        # matters: `idf.py flash` REBUILDS, so a tree that was right at build time
        # and wrong now produces a capture of something nobody asked for. That has
        # happened here once already. Sync, then read the tree back and assert.
        $wantVariant = Get-WantedSdkVariant $tag
        $wantCt = Get-WantedCt $tag
        if (-not $NoSdkSync) {
            & (Join-Path $PSScriptRoot 'sdk_variant.ps1') -Set $wantVariant -Ct $wantCt | Out-Null
        }
        $haveVariant = & (Join-Path $PSScriptRoot 'sdk_variant.ps1') -Verify | Select-Object -Last 1
        if ($haveVariant -ne $wantVariant) {
            throw "${tag}: the SDK bignum port is in state '$haveVariant' but this run measures '$wantVariant'. Fix it with ./sdk_variant.ps1 -Set $wantVariant (or drop -NoSdkSync)."
        }
        Write-Host "$tag : SDK bignum port is '$haveVariant'"

        if (-not $SkipMacCheck) { Assert-Board -Port $usePort -ExpectMac $board.Mac -Tag $tag }

        Write-Host "$tag : flashing $usePort ..."
        idf.py "@$tag.args" -p $usePort -b 460800 flash | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "${tag}: flash failed with exit code $LASTEXITCODE" }

        # Again, AFTER the flash. The flash step is itself a build, so the state
        # that matters is the state across it, not before it -- and the tree is a
        # shared resource that another window can move. If this fires, the image
        # now on the device is not the one the tag names: rebuild and reflash.
        $afterVariant = & (Join-Path $PSScriptRoot 'sdk_variant.ps1') -Verify | Select-Object -Last 1
        if ($afterVariant -ne $wantVariant) {
            throw "${tag}: the SDK bignum port changed to '$afterVariant' during the flash. The device now carries something other than '$wantVariant' -- discard this run."
        }

        Write-Host "$tag : capturing ..."
        $text = Capture-Boot -Port $usePort -TimeoutSec $TimeoutSec -Until '[done]'
        Set-Content -Path (Join-Path $OutDir "$tag.log") -Value $text -NoNewline

        if (-not $text.Contains('[done]')) {
            Write-Warning "${tag}: no [done] marker within ${TimeoutSec}s -- the capture is incomplete and its rows are not trustworthy."
        }
        if ($text.Contains('[fail]')) {
            Write-Warning "${tag}: the device reported [fail]. Read $OutDir\$tag.log before using any of its numbers."
        }
    }
}

# ---- Collate ------------------------------------------------------------
$all = @()
foreach ($f in Get-ChildItem -Path $OutDir -Filter '*.log' -ErrorAction SilentlyContinue) {
    $all += Parse-Run -Tag $f.BaseName -Text (Get-Content $f.FullName -Raw)
}
if ($all.Count -eq 0) {
    Write-Host "No captured runs in $OutDir yet."
    return
}

$csv = Join-Path $OutDir 'results.csv'
$all | Export-Csv -Path $csv -NoTypeInformation
Write-Host "$($all.Count) rows from $((($all | Select-Object -ExpandProperty run) | Sort-Object -Unique).Count) run(s) -> $csv`n"
$all | Format-Table run, bench, bits, mean_us, min_us, max_us, mul_calls_per_op, mul_us_per_op -AutoSize

# ---- Crossover summary --------------------------------------------------
# Doing this by eye off a 400-row table is how a sign change gets missed. For
# every run that carries the paired mpi_mul_hw / mpi_mul_sw rows, print the
# difference per size and interpolate the crossing.
#
# It reports "which side is 384 on" separately from the interpolated bits,
# because that is the part with consequences: only 256, 384, 512 and RSA's 1024+
# ever reach mbedtls_mpi_mul_mpi, so every threshold in (384, 512] routes real
# workloads identically and the exact crossing is decoration.
$xruns = $all | Where-Object { $_.bench -eq 'mpi_mul_hw' } | Select-Object -ExpandProperty run -Unique
foreach ($r in $xruns) {
    $hw = @{}; $sw = @{}
    foreach ($row in ($all | Where-Object { $_.run -eq $r })) {
        if ($row.bench -eq 'mpi_mul_hw') { $hw[[int]$row.bits] = [double]$row.mean_us }
        if ($row.bench -eq 'mpi_mul_sw') { $sw[[int]$row.bits] = [double]$row.mean_us }
    }
    $sizes = $hw.Keys | Where-Object { $sw.ContainsKey($_) } | Sort-Object
    if (-not $sizes) { continue }

    $id = ($all | Where-Object { $_.run -eq $r } | Select-Object -First 1)
    Write-Host "`n--- $r  ($($id.target) @ $($id.cpu_mhz_rt) MHz) ---"
    $rows = foreach ($b in $sizes) {
        [pscustomobject][ordered]@{
            bits     = $b
            hw_us    = [math]::Round($hw[$b], 2)
            sw_us    = [math]::Round($sw[$b], 2)
            'sw-hw'  = [math]::Round($sw[$b] - $hw[$b], 2)
            faster   = if ($sw[$b] -lt $hw[$b]) { 'software' } else { 'hardware' }
        }
    }
    $rows | Format-Table -AutoSize

    # First sign change in (sw - hw), linearly interpolated between the two
    # bracketing sizes. Linear is honest over a 32-bit step and no more is
    # claimed by quoting it to the nearest bit.
    $cross = $null
    for ($i = 1; $i -lt $sizes.Count; $i++) {
        $p = $sizes[$i - 1]; $q = $sizes[$i]
        $dp = $sw[$p] - $hw[$p]; $dq = $sw[$q] - $hw[$q]
        if ($dp -lt 0 -and $dq -ge 0) { $cross = $p + ($q - $p) * ($(-$dp) / ($dq - $dp)); break }
    }
    if ($null -eq $cross) {
        $allSw = ($sizes | Where-Object { $sw[$_] -lt $hw[$_] }).Count
        Write-Host ("crossover: none in {0}..{1} -- {2} wins at every measured size" -f $sizes[0], $sizes[-1],
                    $(if ($allSw -eq $sizes.Count) { 'software' } else { 'hardware' }))
    } else {
        Write-Host ("crossover: ~{0:N0} bits" -f $cross)
        # mbedtls_mpi_core_mla() unrolls by 8 limbs and does the remainder one
        # limb at a time, so the software curve is a SAWTOOTH: 15 limbs (480
        # bits) and 23 (736) cost more than the 16 and 24 above them. A lone
        # sign change can therefore be a tooth rather than a crossing, and
        # reporting it as "the crossover" is how a threshold table ends up
        # naming the wrong number. If software wins again above the crossing,
        # say so instead of pretending the first flip settled it.
        $after = $sizes | Where-Object { $_ -gt $cross -and $sw[$_] -lt $hw[$_] }
        if ($after) {
            Write-Host ("  ...but software wins again at {0} -- that flip is a TOOTH, not a crossing. True crossover is above {1} bits." -f
                        ($after -join ', '), $sizes[-1])
        }
    }
    if ($hw.ContainsKey(384)) {
        $side = if ($sw[384] -lt $hw[384]) { 'SOFTWARE' } else { 'HARDWARE' }
        $pct = [math]::Round([math]::Abs($sw[384] - $hw[384]) / [math]::Max($sw[384], $hw[384]) * 100, 1)
        Write-Host ("384 bits -> {0} is faster, by {1}%   <- the part that changes a routing decision" -f $side, $pct)
    }
}
