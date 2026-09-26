<#
Install upstream osmocom core conv sources into third_party/osmo_viterbi
Usage:
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_osmo_core.ps1 -SourceDir C:\path\to\osmocom-core

Behavior:
- Looks for conv.c and conv.h (case-insensitive) in the provided SourceDir.
- Copies them to third_party/osmo_viterbi/
- If a local adapter `osmo_conv.c` exists, it will be renamed to `osmo_conv.adapter_bak.c` (safe backup).
- Prints next CMake build commands to run.
#>
param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [switch]$DryRun
)
if (-not (Test-Path $SourceDir)) {
    Write-Error "SourceDir '$SourceDir' does not exist."
    exit 2
}
$dest = Join-Path $PSScriptRoot "..\third_party\osmo_viterbi" | Resolve-Path -Relative
$dest = (Resolve-Path -Path (Join-Path $PSScriptRoot "..\third_party\osmo_viterbi")).Path
Write-Host "Destination: $dest"
# find files
$files = Get-ChildItem -Path $SourceDir -Recurse -Include conv.c,conv.h -File -ErrorAction SilentlyContinue
if ($files.Count -eq 0) {
    Write-Host "No conv.c/conv.h found under $SourceDir. Look for files named 'conv.c' and 'conv.h'." -ForegroundColor Yellow
    exit 3
}
foreach ($f in $files) {
    $target = Join-Path $dest $f.Name
    Write-Host "Found: $($f.FullName) -> $target"
    if ($DryRun) { continue }
    Copy-Item -Path $f.FullName -Destination $target -Force
}
# backup existing adapter if present
$adapter = Join-Path $dest "osmo_conv.c"
if ((Test-Path $adapter)) {
    $bak = Join-Path $dest "osmo_conv.adapter_bak.c"
    if (-not (Test-Path $bak)) {
        Write-Host "Backing up existing adapter: osmo_conv.c -> osmo_conv.adapter_bak.c"
        if (-not $DryRun) { Move-Item -Path $adapter -Destination $bak }
    } else {
        Write-Host "Backup already exists: $bak" -ForegroundColor Yellow
    }
}
Write-Host "Installed upstream conv files to $dest"
Write-Host "Next steps (example):"
Write-Host "  cmake -S . -B build -DUSE_OSMO_VITERBI=ON"
Write-Host "  cmake --build build --config Release --target dsd-neo_test_tetra_depunct_regression"
Write-Host "If you want, run this script with -DryRun first to verify which files will be copied."
Write-Host "If upstream conv files require additional helpers, copy them into $dest as well and re-run the build."
Write-Host "If you want me to run the build locally after you place the files, tell me and I will run cmake and build the regression target."