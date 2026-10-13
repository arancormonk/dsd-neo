param(
    [string]$Archive = (Join-Path $PSScriptRoot 'en_30039502v010301p0.zip'),
    [string]$OutputDir = (Join-Path $PSScriptRoot 'reference'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
    throw "ETSI archive not found: $Archive. Download en_30039502v010301p0.zip from ETSI and place it in third_party\tetra_acelp."
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDir)
$expectedRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
if (-not $resolvedOutput.StartsWith($expectedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to write outside third_party\tetra_acelp: $resolvedOutput"
}

if (Test-Path -LiteralPath $OutputDir) {
    if (-not $Force) {
        throw "Output already exists: $OutputDir. Re-run with -Force to replace it."
    }
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}

New-Item -ItemType Directory -Path $OutputDir | Out-Null
Expand-Archive -LiteralPath $Archive -DestinationPath $OutputDir -Force

$cCode = Get-ChildItem -LiteralPath $OutputDir -Directory -Recurse |
    Where-Object { $_.Name -eq 'C-CODE' } |
    Select-Object -First 1

if ($null -eq $cCode) {
    throw "Archive extracted, but no C-CODE directory was found under $OutputDir."
}

$target = Join-Path $OutputDir 'C-CODE'
if ($cCode.FullName -ne $target) {
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
    Move-Item -LiteralPath $cCode.FullName -Destination $target
}

if (-not (Test-Path -LiteralPath (Join-Path $target 'SOURCE.H') -PathType Leaf)) {
    throw "Prepared C-CODE directory is missing SOURCE.H: $target"
}

Write-Host "Prepared ETSI TETRA ACELP decoder sources:"
Write-Host $target
Write-Host ""
Write-Host "Build adapter:"
Write-Host "cmake -S tools/tetra/acelp_adapter -B build/tetra-acelp-adapter -DETSI_TETRA_C_CODE=""$target"""
Write-Host "cmake --build build/tetra-acelp-adapter --config Release"
