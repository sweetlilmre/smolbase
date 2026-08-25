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
    [string]$Port = 'COM5',
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
    [switch]$ParseOnly
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

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

        if (-not $Flash) {
            Write-Host "$tag : built and config-checked. Re-run with -Flash to flash and capture (this ERASES the smolbase firmware)."
            continue
        }

        Write-Host "$tag : flashing $Port ..."
        idf.py "@$tag.args" -p $Port -b 460800 flash | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "${tag}: flash failed with exit code $LASTEXITCODE" }

        Write-Host "$tag : capturing ..."
        $text = Capture-Boot -Port $Port -TimeoutSec $TimeoutSec -Until '[done]'
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
