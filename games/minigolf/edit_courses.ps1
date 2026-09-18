param([string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $projectRoot $BuildDir }
$editorBuild = [System.IO.Path]::GetFullPath($BuildDir)
$cache = Join-Path $editorBuild 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) { throw 'Configure an application build first, for example: cmake --preset app-sim' }
$cmakeEntry = Get-Content -LiteralPath $cache | Select-String '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
if (-not $cmakeEntry) { throw 'CMake executable was not found in the build cache.' }
$cmakeExe = $cmakeEntry.Matches[0].Groups[1].Value
& $cmakeExe --build $editorBuild --target TreblotronCourseEditor --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Course editor build failed; the previous executable was not launched.' }
& (Join-Path $editorBuild 'bin/TreblotronCourseEditor.exe')
exit $LASTEXITCODE
