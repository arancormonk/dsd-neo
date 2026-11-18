# SPDX-License-Identifier: GPL-3.0-or-later
# Stages the Windows release tree that the ZIP is made from:
#   bin\                          dsd-neo.exe, SoapySDRUtil.exe and every DLL they and
#                                 the SoapySDR modules load
#   lib\SoapySDR\modules<ABI>\    the SoapySDR modules (exactly the expected ones)
#   share\doc\dsd-neo\            the project's and every bundled component's notices
# windows-pr's Release build and windows-ci's release package both run it, and
# verify_release_tree.ps1 checks what it leaves. Relative paths in the notice list
# below are from the repository root.
[CmdletBinding()]
param(
    # The configured CMake build directory (dsd-neo.exe under apps\dsd-cli).
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [string]$StageDir,

    # vcpkg's installed tree for the target triplet (vcpkg_installed\<triplet>).
    [Parameter(Mandatory = $true)]
    [string]$InstalledDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Import-Module (Join-Path $PSScriptRoot 'release_tree_checks.psm1') -Force

# SoapyAirspy, SoapyAirspyHF and SoapyHackRF (vcpkg-ports/soapy-*). Anything else in
# the module folder is an error, so a dependency change cannot add a module unseen.
$expectedModules = @('airspySupport.dll', 'airspyhfSupport.dll', 'HackRFSupport.dll')
# Windows-only notices of the SoapySDR load path: the modules, the libraries they
# need, and the libusb and PThreads4W those libraries load (license texts and the
# holders and contributors their sources name). SoapySDR's own notices come with the
# legal component, from LICENSES/.
$soapyNotices = [ordered]@{
    'vcpkg-ports/soapy-airspy/copyright' = 'SoapyAirspy-MIT.txt'
    'vcpkg-ports/soapy-hackrf/copyright' = 'SoapyHackRF-MIT.txt'
    'vcpkg-ports/soapy-airspyhf/copyright' = 'SoapyAirspyHF-MIT.txt'
    'vcpkg-ports/hackrf/copyright' = 'libhackrf-BSD-3-Clause.txt'
    'vcpkg-ports/airspyhf/copyright' = 'libairspyhf-BSD-3-Clause.txt'
    'android/third_party/libusb/COPYING' = 'libusb-LGPL-2.1-or-later.txt'
    'packaging/windows/PThreads4W-LICENSE.txt' = 'PThreads4W-Apache-2.0.txt'
    'packaging/windows/PThreads4W-NOTICE.txt' = 'PThreads4W-NOTICE.txt'
    'packaging/windows/PThreads4W-CONTRIBUTORS.txt' = 'PThreads4W-CONTRIBUTORS.txt'
    'packaging/windows/libusb-COPYRIGHT.txt' = 'libusb-COPYRIGHT.txt'
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$installed = (Resolve-Path -LiteralPath $InstalledDir).Path
# A used folder would mix two builds: stage_runtime_dlls.ps1 keeps a DLL already in
# bin\ rather than copy the one vcpkg installed, so a new module could ship beside an
# older library. Only an absent or empty folder is staged into.
if ((Test-Path -LiteralPath $StageDir) -and @(Get-ChildItem -LiteralPath $StageDir -Force).Count -gt 0) {
    throw "The staging folder is not empty: $((Resolve-Path -LiteralPath $StageDir).Path)"
}
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
$stage = (Resolve-Path -LiteralPath $StageDir).Path
$stageRuntimeDlls = Join-Path $PSScriptRoot 'stage_runtime_dlls.ps1'

$binDir = Join-Path $stage 'bin'
$docDir = Join-Path $stage 'share/doc/dsd-neo'
$licenseDir = Join-Path $docDir 'licenses'
New-Item -ItemType Directory -Force -Path $binDir, $docDir, $licenseDir | Out-Null
$searchDirs = @(Join-Path $installed 'bin')
if (!(Test-Path -LiteralPath $searchDirs[0])) {
    throw "vcpkg bin directory not found: $($searchDirs[0])"
}

# dsd-neo.exe and the DLLs it loads.
$exe = Get-ChildItem -LiteralPath (Join-Path $build 'apps/dsd-cli') -Recurse -File -Filter 'dsd-neo.exe' |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $exe) {
    throw "dsd-neo.exe not found under $(Join-Path $build 'apps/dsd-cli')"
}
Copy-Item -Force -LiteralPath $exe -Destination $binDir
& $stageRuntimeDlls -TargetBinary (Join-Path $binDir 'dsd-neo.exe') -DestinationDir $binDir -SearchDirs $searchDirs

# The SoapySDR modules, in the folder SoapySDR searches relative to SoapySDR.dll's
# parent folder: lib\SoapySDR\modules<ABI>. A module's DLLs go to bin\: Windows looks
# for them where it looks for dsd-neo.exe's, never in the module's own folder.
$versionHeader = Join-Path $installed 'include/SoapySDR/Version.h'
if (!(Test-Path -LiteralPath $versionHeader)) {
    throw "SoapySDR version header not found: $versionHeader"
}
$abi = [regex]::Match((Get-Content -Raw -LiteralPath $versionHeader),
    '#define\s+SOAPY_SDR_ABI_VERSION\s+"([0-9]+\.[0-9]+(-[A-Za-z0-9]+)?)"')
if (-not $abi.Success) {
    throw "No SOAPY_SDR_ABI_VERSION in $versionHeader"
}
$moduleSubdir = 'lib/SoapySDR/modules' + $abi.Groups[1].Value
$moduleSource = Join-Path $installed $moduleSubdir
if (!(Test-Path -LiteralPath $moduleSource)) {
    throw "SoapySDR module folder not found: $moduleSource"
}
$modules = @(Get-ChildItem -LiteralPath $moduleSource -File -Filter '*.dll' | Sort-Object -Property Name)
$found = @($modules | Select-Object -ExpandProperty Name)
if ((($found | Sort-Object) -join ', ') -cne (($expectedModules | Sort-Object) -join ', ')) {
    throw "Expected exactly the SoapySDR modules $($expectedModules -join ', ') in $moduleSource; found $($found -join ', ')"
}
$moduleDir = Join-Path $stage $moduleSubdir
New-Item -ItemType Directory -Force -Path $moduleDir | Out-Null
foreach ($module in $modules) {
    Copy-Item -Force -LiteralPath $module.FullName -Destination $moduleDir
    & $stageRuntimeDlls -TargetBinary (Join-Path $moduleDir $module.Name) -DestinationDir $binDir -SearchDirs $searchDirs
}

# SoapySDRUtil, from the soapysdr overlay's tools feature. In bin\ next to
# SoapySDR.dll it finds the same modules dsd-neo.exe finds.
$util = Join-Path $installed 'tools/soapysdr/SoapySDRUtil.exe'
if (!(Test-Path -LiteralPath $util)) {
    throw "SoapySDRUtil.exe not found: $util"
}
Copy-Item -Force -LiteralPath $util -Destination $binDir
& $stageRuntimeDlls -TargetBinary (Join-Path $binDir 'SoapySDRUtil.exe') -DestinationDir $binDir -SearchDirs $searchDirs

Push-Location -LiteralPath $repoRoot
try {
    cmake --install $build --prefix $stage --component legal
    if ($LASTEXITCODE -ne 0) { throw "Legal notice installation failed" }
    Copy-Item -Force LICENSE,COPYRIGHT,THIRD_PARTY.md $docDir -ErrorAction Stop
    Copy-Item -Force 'src/third_party/ezpwd/lesser.txt' (Join-Path $licenseDir 'ezpwd-LGPL-2.1-or-later.txt') -ErrorAction Stop
    Copy-Item -Force (Join-Path $build 'pffft-licenses/pffft-LICENSE.txt'),(Join-Path $build 'pffft-licenses/pffft-NOTICES.txt') $licenseDir -ErrorAction Stop
    Copy-Item -Force (Join-Path $build 'tetra_crypto-licenses/tetra_crypto-LICENSE.txt') $licenseDir -ErrorAction Stop
    Copy-Item -Force 'android/third_party/libairspy/LICENSE' (Join-Path $licenseDir 'libairspy-LICENSE.txt') -ErrorAction Stop
    # The bundled librtlsdr is GPL-2.0-or-later and built by the vcpkg overlay with the dsd-neo patches.
    Copy-Item -Force 'android/third_party/librtlsdr/COPYING' (Join-Path $licenseDir 'librtlsdr-GPL-2.0-or-later.txt') -ErrorAction Stop
    Copy-Item -Force 'packaging/librtlsdr-PATCHES.txt' (Join-Path $licenseDir 'librtlsdr-PATCHES.txt') -ErrorAction Stop
    foreach ($source in $soapyNotices.Keys) {
        Copy-Item -Force $source (Join-Path $licenseDir $soapyNotices[$source]) -ErrorAction Stop
    }
} finally {
    Pop-Location
}

Assert-ReleaseNotices -DocDir $docDir
Write-Host "Staged release tree: $stage"
