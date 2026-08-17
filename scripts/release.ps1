<#
.SYNOPSIS
    Build every Dartmatic release artifact and collect them in one directory.

.DESCRIPTION
    Four configurations, each with one audience:

      Dartmatic-0.1.0-AMD64.exe          single PC: cameras and GPU inference here
      Dartmatic-Server-0.1.0-AMD64.exe   the box doing inference for a Pi
      Dartmatic-Demo-0.1.0-AMD64.zip     no hardware; portable, no installer
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

$configs = @{
    app    = @{ Preset = "app-local-windows"; Dir = "build-app-local-windows"; Exe = "Dartmatic.exe" }
    server = @{ Preset = "server-directml";   Dir = "build-server-directml";   Exe = "Dartmatic_Server.exe" }
    demo   = @{ Preset = "app-demo";          Dir = "build-app-demo";          Exe = "Dartmatic.exe" }
}

# NSIS is only needed by the two installers; the demo is a zip.
if (($Only | Where-Object { $_ -ne "demo" }) -and -not (Get-Command makensis -ErrorAction SilentlyContinue)) {
    throw "makensis is not on PATH. Install NSIS (winget install NSIS.NSIS) or build -Only demo."
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path
$built = @()

foreach ($name in $Only) {
    $cfg = $configs[$name]
    if (-not $cfg) { throw "Unknown target '$name'. Valid: app, server, demo." }

    Write-Host "`n=== $name ($($cfg.Preset)) ===" -ForegroundColor Cyan

    cmake --preset $cfg.Preset | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $($cfg.Preset)" }

    cmake --build $cfg.Dir | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed for $($cfg.Preset)" }

    # A release binary that only runs where it was built is the failure mode
    # this whole exercise exists to prevent, so it is checked every time rather
    # than trusted. A stripped-down PATH stands in for a machine that has never
    # had a compiler on it.
    if (-not $SkipVerify) {
        $exe = Join-Path $repo "$($cfg.Dir)\bin\$($cfg.Exe)"
        $saved = $env:PATH
        try {
            $env:PATH = "$env:SystemRoot\system32;$env:SystemRoot"
            if ($name -eq "server") {
                & $exe --version | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "$($cfg.Exe) will not start without the toolchain on PATH" }
            } else {
                $p = Start-Process -FilePath $exe -PassThru
                Start-Sleep -Seconds 5
                $p.Refresh()
                if ($p.HasExited) { throw "$($cfg.Exe) exited immediately without the toolchain on PATH (code $($p.ExitCode))" }
                $p.Kill()
            }
            Write-Host "  starts on a clean PATH" -ForegroundColor Green
        } finally {
            $env:PATH = $saved
        }
    }

    Push-Location $cfg.Dir
    try {
        cpack | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cpack failed for $($cfg.Preset)" }
    } finally {
        Pop-Location
    }

    Get-ChildItem "$($cfg.Dir)\Dartmatic*" -Include *.exe, *.zip, *.sha256 -File |
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
