# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Artifact
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $vswherePaths = @()
    if (${env:ProgramFiles(x86)}) {
        $vswherePaths += Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if ($env:ProgramFiles) {
        $vswherePaths += Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
    }

    foreach ($vswhere in $vswherePaths) {
        if (-not (Test-Path -LiteralPath $vswhere)) {
            continue
        }

        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installationPath) {
            continue
        }

        $toolRoot = Join-Path $installationPath "VC\Tools\MSVC"
        if (-not (Test-Path -LiteralPath $toolRoot)) {
            continue
        }

        $candidate = Get-ChildItem -LiteralPath $toolRoot -Filter dumpbin.exe -Recurse |
            Where-Object { $_.FullName -match "\\Hostx64\\x64\\dumpbin\.exe$" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return $candidate.FullName
        }
    }

    throw "dumpbin.exe not found. Run from a Visual Studio developer shell or install MSVC build tools."
}

if (-not (Test-Path -LiteralPath $Artifact)) {
    throw "release artifact not found: $Artifact"
}

$dumpbin = Resolve-Dumpbin
$headers = & $dumpbin /headers $Artifact 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $Artifact"
}

if ($headers -notmatch "Dynamic base") {
    throw "release artifact is missing PE Dynamic base (ASLR): $Artifact"
}
if ($headers -notmatch "NX compatible") {
    throw "release artifact is missing PE NX compatible: $Artifact"
}

$is64Bit = $headers -match "machine \((x64|ARM64|ARM64EC)\)" -or $headers -match "(8664|AA64|A64E) machine"
if ($is64Bit -and $headers -notmatch "High Entropy Virtual Addresses") {
    throw "64-bit release artifact is missing PE High Entropy Virtual Addresses: $Artifact"
}

Write-Host "Windows PE hardening OK: $Artifact"
