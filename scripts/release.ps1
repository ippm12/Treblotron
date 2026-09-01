<#
.SYNOPSIS
    Build every Treblotron release artifact and collect them in one directory.

.DESCRIPTION
    Four configurations, each with one audience:

      Treblotron-0.1.0-AMD64.exe          single PC: cameras and GPU inference here
      Treblotron-Server-0.1.0-AMD64.exe   the box doing inference for a Pi
      Treblotron-Demo-0.1.0-AMD64.zip     no hardware; portable, no installer
      (Pi client)                        built on the Pi itself, see docs/SETUP.md

    They are separate CMake builds rather than components of one installer,
    because they disagree about SDL, about OpenCV's module list and about
    whether an inference backend exists at all.

    Requires NSIS on PATH for the two installers (winget install NSIS.NSIS).
    The demo zip needs nothing extra.

.PARAMETER Output
    Where finished artifacts are copied. Defaults to dist/ in the repo root.

.PARAMETER Only
    Build a subset, e.g. -Only demo,server. Default is everything.

.PARAMETER SkipVerify
    Skip the post-build check that binaries start with the toolchain off PATH.
    Do not use this for a real release: that check is the only thing standing
    between you and shipping something that runs nowhere but this machine.
#>
[CmdletBinding()]
param(
    [string]   $Output,
    [string[]] $Only   = @("app", "server", "demo"),
    [switch]   $SkipVerify
)

$ErrorActionPreference = "Stop"

# Resolved in the body, not as a parameter default: $PSScriptRoot is empty
# inside a param() block when the script is launched with `powershell -File`,
# which silently turned the default into "\..\dist" and wrote a release to the
# root of C:.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo      = (Resolve-Path (Join-Path $scriptDir "..")).Path
if (-not $Output) { $Output = Join-Path $repo "dist" }
Set-Location $repo

# `powershell -File script.ps1 -Only app,demo` hands the whole thing over as a
# single string rather than binding it as an array, so split it back apart.
# Someone typing a comma-separated list means a list either way.
$Only = $Only | ForEach-Object { $_ -split "," } | Where-Object { $_ } | ForEach-Object { $_.Trim() }

$configs = @{
    app    = @{ Preset = "app-local-windows"; Dir = "build-app-local-windows"; Exe = "Treblotron.exe" }
    server = @{ Preset = "server-directml";   Dir = "build-server-directml";   Exe = "Treblotron_Server.exe" }
    demo   = @{ Preset = "app-demo";          Dir = "build-app-demo";          Exe = "Treblotron.exe" }
}

# NSIS is only needed by the two installers; the demo is a zip.
#
# Its own installer does not add itself to PATH, so looking only there would
# send someone editing environment variables for a tool that already records
# where it lives. Check the registry and the usual directories too, and put it
# on PATH for this process so CPack finds it by name.
function Resolve-MakeNsis {
    $found = Get-Command makensis -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }

    foreach ($key in 'HKLM:\SOFTWARE\NSIS', 'HKLM:\SOFTWARE\WOW6432Node\NSIS') {
        $root = (Get-ItemProperty $key -ErrorAction SilentlyContinue).'(default)'
        if ($root -and (Test-Path "$root\makensis.exe")) { return "$root\makensis.exe" }
    }
    foreach ($p in "$env:ProgramFiles\NSIS\makensis.exe", "${env:ProgramFiles(x86)}\NSIS\makensis.exe") {
        if (Test-Path $p) { return $p }
    }
    return $null
}

if ($Only | Where-Object { $_ -ne "demo" }) {
    $nsis = Resolve-MakeNsis
    if (-not $nsis) {
        throw "NSIS not found. Install it (winget install NSIS.NSIS) or build -Only demo."
    }
    $env:PATH = "$(Split-Path -Parent $nsis);$env:PATH"
    Write-Host "Using NSIS at $nsis" -ForegroundColor DarkGray
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path
$built = @()

foreach ($name in $Only) {
    $cfg = $configs[$name]
    if (-not $cfg) { throw "Unknown target '$name'. Valid: app, server, demo." }

    Write-Host "`n=== $name ($($cfg.Preset)) ===" -ForegroundColor Cyan

    # Output is captured rather than discarded. Piping the build to Out-Null
    # loses the compiler error along with everything else, leaving "build
    # failed" and nothing to act on -- and a cold build compiles OpenCV from
    # source, so it also means twenty silent minutes that look like a hang.
    $log = Join-Path ([System.IO.Path]::GetTempPath()) "treblotron-$name.log"
    $lines = [System.Collections.Generic.List[string]]::new()

    function Invoke-Stage([string] $what, [scriptblock] $cmd) {
        $script:lines.Clear()
        $script:lastReported = 0

        # Announced up front: the configure stage emits no [n/m] progress and
        # spends about a minute in OpenCV, which is long enough to read as a
        # hang if nothing has been printed.
        Write-Host ("  {0}..." -f $what) -NoNewline

        # CMake writes message() output to stderr and ninja writes progress to
        # stdout, so both streams are wanted. Merging them with 2>&1 turns every
        # stderr line into an ErrorRecord, though, and with
        # $ErrorActionPreference = "Stop" the first one -- "Including debug
        # library", before anything has gone wrong -- aborts the script.
        #
        # Relax the preference for the duration and judge the outcome by the
        # exit code, which is the only thing a native command actually reports
        # failure through.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $cmd 2>&1 | ForEach-Object {
                $text = if ($_ -is [System.Management.Automation.ErrorRecord]) {
                            $_.ToString()
                        } else {
                            [string] $_
                        }
                $script:lines.Add($text)
                # Ninja prefixes each step with [done/total]; report every 5%.
                if ($text -match '^\[(\d+)/(\d+)\]') {
                    $cur = [int] $Matches[1]
                    $tot = [int] $Matches[2]
                    if ($tot -gt 0 -and ($cur - $script:lastReported) -ge [math]::Max(1, [int]($tot / 20))) {
                        $script:lastReported = $cur
                        $pct = [int](100 * $cur / $tot)
                        Write-Host ("`r  {0}  {1,5}/{2}  {3,3}%   " -f $what, $cur, $tot, $pct) -NoNewline
                    }
                }
            }
        } finally {
            $ErrorActionPreference = $prev
        }

        Write-Host ""
        if ($LASTEXITCODE -ne 0) {
            Set-Content -Path $log -Value $script:lines -Encoding utf8
            $script:lines | Select-Object -Last 30 | ForEach-Object { Write-Host $_ -ForegroundColor Red }
            throw "$what failed for $($cfg.Preset) -- full output: $log"
        }
    }

    Invoke-Stage "configure" { cmake --preset $cfg.Preset }
    Invoke-Stage "build"     { cmake --build $cfg.Dir }

    # A release binary that only runs where it was built is the failure mode
    # this whole exercise exists to prevent, so it is checked every time rather
    # than trusted. A stripped-down PATH stands in for a machine that has never
    # had a compiler on it.
    #
    # The check runs against a real install tree, not the build directory. The
    # two are not the same set of files -- the build tree is assembled by
    # POST_BUILD copies, the installed one by install() rules -- and it is the
    # installed one that ships. Testing the build tree can pass while the
    # installer is missing a DLL, which is precisely the bug worth catching.
    if (-not $SkipVerify) {
        $stage = Join-Path ([System.IO.Path]::GetTempPath()) "treblotron-verify-$name"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }

        cmake --install $cfg.Dir --prefix $stage | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "install to staging failed for $($cfg.Preset)" }

        $exe = Join-Path $stage "bin\$($cfg.Exe)"
        if (-not (Test-Path $exe)) { throw "$($cfg.Exe) is not in the install tree at $stage" }

        # Resolve the import graph rather than launching. A missing DLL raises a
        # modal "code execution cannot proceed" dialog, and a process sitting on
        # that dialog has not exited -- so launching reports success and hangs an
        # unattended build. See cmake/check_runtime_deps.cmake.
        try {
            cmake "-DBIN_DIR=$stage/bin" "-DEXECUTABLES=$($cfg.Exe)" `
                  -P (Join-Path $repo "cmake\check_runtime_deps.cmake")
            if ($LASTEXITCODE -ne 0) {
                throw "$($cfg.Exe) depends on libraries the installer does not ship"
            }
            Write-Host "  installed tree is self-contained" -ForegroundColor Green
        } finally {
            Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Push-Location $cfg.Dir
    try {
        cpack | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cpack failed for $($cfg.Preset)" }
    } finally {
        Pop-Location
    }

    Get-ChildItem "$($cfg.Dir)\Treblotron*" -Include *.exe, *.zip, *.sha256 -File |
        ForEach-Object {
            Copy-Item $_.FullName $Output -Force
            $built += [pscustomobject]@{
                Artifact = $_.Name
                SizeMB   = [math]::Round($_.Length / 1MB, 1)
            }
        }
}

Write-Host "`n=== artifacts in $Output ===" -ForegroundColor Cyan
$built | Where-Object { $_.Artifact -notlike "*.sha256" } | Format-Table -AutoSize

Write-Host "The Pi client is built on the Pi; see docs/SETUP.md." -ForegroundColor DarkGray
