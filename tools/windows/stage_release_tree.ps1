# SPDX-License-Identifier: GPL-3.0-or-later
# Stages the Windows release tree that the ZIP is made from:
#   bin\                          dsd-neo.exe, SoapySDRUtil.exe and every DLL they and
#                                 the SoapySDR modules load
#   lib\SoapySDR\modules<ABI>\    the SoapySDR modules (exactly the expected ones)
#   share\doc\dsd-neo\            the project's notices and those of the components of
#                                 every file in bin\ and lib\ (Get-ReleaseBinaryRecords:
#                                 the windows_release of each record in
#                                 legal/components.json)
# windows-pr's Release build and windows-ci's release package both run it, and
# verify_release_tree.ps1 checks what it leaves.
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

# SoapyAirspy and SoapyHackRF (vcpkg-ports/soapy-*). Anything else in the module
# folder is an error, so a dependency change cannot add a module unseen.
$expectedModules = @('airspySupport.dll', 'HackRFSupport.dll')

$repoRoot = (Resolve-Path -LiteralPath (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$installed = (Resolve-Path -LiteralPath $InstalledDir).Path

# The Visual C++ runtime the release carries must be that of the toolset that built it
# (legal/msvc-runtime.md). stage_runtime_dlls.ps1 takes it from VCToolsRedistDir, the
# runtime folder of the developer shell's toolset, so the build's compilers must be that
# toolset's: under VCToolsInstallDir, which the same shell sets.
function Assert-BuildToolset {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Build
    )

    foreach ($variable in @('VCToolsInstallDir', 'VCToolsRedistDir')) {
        if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($variable))) {
            throw "$variable is not set: run from the Visual Studio developer shell of the toolset that built $Build"
        }
    }
    $toolset = $env:VCToolsInstallDir.Trim().Replace('\', '/').TrimEnd('/') + '/'
    $cache = Join-Path $Build 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cache -PathType Leaf)) {
        throw "No CMakeCache.txt in $Build, so the toolset that built it is unknown"
    }
    $lines = @(Get-Content -LiteralPath $cache)
    foreach ($language in @('C', 'CXX')) {
        $entries = @($lines | Where-Object { $_ -cmatch "^CMAKE_${language}_COMPILER:[A-Z]+=" })
        if ($entries.Count -ne 1) {
            throw "$cache names no CMAKE_${language}_COMPILER, so the toolset that built $Build is unknown"
        }
        $compiler = $entries[0].Substring($entries[0].IndexOf('=') + 1).Trim()
        if (-not $compiler.Replace('\', '/').StartsWith($toolset, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "$Build was built with $compiler, not with the toolset of this developer shell ($env:VCToolsInstallDir), whose Visual C++ runtime the release would carry"
        }
    }
}

Assert-BuildToolset -Build $build

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
$modules = @(Get-ChildItem -LiteralPath $moduleSource -File -Force -Filter '*.dll' | Sort-Object -Property Name)
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
} finally {
    Pop-Location
}

# The notices of the components of every file staged in bin\ and lib\, from where
# their records say: the repository (repo:), the build tree (build:, the downloaded
# PFFFT's and TETRA_crypto's), the copyright file a vcpkg port installs (vcpkg:<port>,
# share\<port>\copyright) or the legal component installed above (legal-component:
# mbelib's notices, LICENSES\ and legal\). A file no record lists stops the staging.
$componentsPath = Join-Path $repoRoot 'legal/components.json'
$records = Get-ReleaseBinaryRecords -ComponentsPath $componentsPath
foreach ($file in @(Get-ReleaseTreeFiles -TreeRoot $stage)) {
    if (-not $records.ContainsKey($file.Name)) {
        throw "No component record in $componentsPath lists $($file.FullName) (windows_release), so its notices are unknown: add the record before a release carries it"
    }
    foreach ($record in $records[$file.Name]) {
        foreach ($notice in $record.Notices.Keys) {
            $from = $record.Notices[$notice]
            if ($from -eq 'legal-component') {
                continue
            }
            if ($from -match '^(repo|build):(.+)$') {
                $sourceRoot = if ($Matches[1] -eq 'repo') { $repoRoot } else { $build }
                $relative = $Matches[2]
                # A path inside its root: no drive or root, and no empty, '.' or '..' part.
                $parts = @($relative -split '[/\\]')
                if ($relative.Contains(':') -or @($parts | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
                    throw "The notice $notice of $($record.Component) names a source outside its root: '$from'"
                }
                $source = Join-Path $sourceRoot $relative
            } elseif ($from -match '^vcpkg:([a-z0-9-]+)$') {
                $source = Join-Path $installed "share/$($Matches[1])/copyright"
            } else {
                throw "The notice $notice of $($record.Component) names a source staging does not know: '$from'"
            }
            if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "The notice $notice of $($record.Component) comes from $source, which is not a file"
            }
            $destination = Join-Path $docDir $notice
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            Copy-Item -Force -LiteralPath $source -Destination $destination -ErrorAction Stop
        }
    }
}

Assert-ReleaseNotices -DocDir $docDir
Assert-ReleaseBinaryRecords -TreeRoot $stage -ComponentsPath (Join-Path $docDir 'legal/components.json')
Write-Host "Staged release tree: $stage"
