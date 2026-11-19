# SPDX-License-Identifier: GPL-3.0-or-later
# Stages the Windows release tree that the ZIP is made from:
#   bin\                          dsd-neo.exe, SoapySDRUtil.exe and every DLL they and
#                                 the SoapySDR modules load
#   lib\SoapySDR\modules<ABI>\    the SoapySDR modules (exactly the expected ones)
#   share\doc\dsd-neo\            the project's notices and those of the components of
#                                 every file in bin\ and lib\ (Get-ReleaseBinaryRecords:
#                                 the windows_release of each record in
#                                 legal/components.json), and notices.sha256, the
#                                 SHA-256 of each of them no record reviews
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
# toolset's: under VCToolsInstallDir, which the same shell sets, and that runtime folder
# must be the one the same installation pairs with the toolset
# (Assert-ToolsetRedistributable). Paths are compared as the file system resolves them,
# so a cache that names the toolset through another spelling of the same folder (a short
# 8.3 name, a link or a junction) passes, and a compiler that only appears to be under it
# does not.
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
    $toolset = Resolve-CanonicalPath $env:VCToolsInstallDir
    foreach ($language in @('C', 'CXX')) {
        $compiler = Get-CMakeCacheValue -Build $Build -Name "CMAKE_${language}_COMPILER"
        try {
            $resolved = Resolve-CanonicalPath $compiler
        } catch {
            throw "$Build was built with $compiler, which is not on this machine, so it is not the toolset of this developer shell ($env:VCToolsInstallDir), whose Visual C++ runtime the release would carry"
        }
        if (-not (Test-PathInside -Path $resolved -Folder $toolset)) {
            throw "$Build was built with $compiler ($resolved), not with the toolset of this developer shell ($env:VCToolsInstallDir), whose Visual C++ runtime the release would carry"
        }
    }
    Assert-ToolsetRedistributable -ToolsInstallDir $env:VCToolsInstallDir -RedistDir $env:VCToolsRedistDir | Out-Null
}

# The vcpkg tree must be the one the build was configured with: the triplet folder of
# VCPKG_INSTALLED_DIR named by VCPKG_TARGET_TRIPLET in the build's CMakeCache.txt. Another
# workspace's tree of the same triplet would pair this build's dsd-neo.exe with another
# build's modules and DLLs.
function Assert-BuildInstalledTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Build,

        [Parameter(Mandatory = $true)]
        [string]$Installed
    )

    $root = Get-CMakeCacheValue -Build $Build -Name 'VCPKG_INSTALLED_DIR'
    $triplet = Get-CMakeCacheValue -Build $Build -Name 'VCPKG_TARGET_TRIPLET'
    if ($triplet -notmatch '^[A-Za-z0-9_-]+$' -or -not [System.IO.Path]::IsPathRooted($root.Replace('\', '/'))) {
        throw "The CMakeCache.txt of $Build names VCPKG_INSTALLED_DIR '$root' and VCPKG_TARGET_TRIPLET '$triplet', not a vcpkg tree"
    }
    $expected = $root.TrimEnd('/', '\') + '/' + $triplet
    try {
        $built = Resolve-CanonicalPath $expected
    } catch {
        throw "$Build was built against the vcpkg tree $expected, which is not on this machine"
    }
    if (-not (Test-SamePath -Path (Resolve-CanonicalPath $Installed) -Other $built)) {
        throw "$Installed is not the vcpkg tree $Build was built against ($expected, from VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET in its CMakeCache.txt): the release would pair its dsd-neo.exe with another build's modules and DLLs"
    }
}

Assert-BuildToolset -Build $build
Assert-BuildInstalledTree -Build $build -Installed $installed

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
# mbelib's notices, LICENSES\ and legal\). A file no record lists stops the staging, and
# so does a notice two records take from different files.
$componentsPath = Join-Path $repoRoot 'legal/components.json'
$records = Get-ReleaseBinaryRecords -ComponentsPath $componentsPath
$stagedFrom = @{}
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
            # Many files share a record (dsd-neo.exe's) or a notice (LICENSE): each notice is
            # copied once, and a second source for it must hold the same text.
            if ($stagedFrom.ContainsKey($notice)) {
                $first = $stagedFrom[$notice]
                if ($first -ne $source -and (Get-NoticeHash -Path $first) -ne (Get-NoticeHash -Path $source)) {
                    throw "The notice $notice comes from both $first and $source, whose texts differ"
                }
                continue
            }
            $stagedFrom[$notice] = $source
            $destination = Join-Path $docDir $notice
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            Copy-Item -Force -LiteralPath $source -Destination $destination -ErrorAction Stop
        }
    }
}

# The checks below read the inventory the tree ships, which must be the one the staging
# read. Then the tree records the SHA-256 of each notice no record reviews (those from
# the build tree, vcpkg and the legal component), so verify_release_tree.ps1 can check
# them in the staged tree and in the unpacked ZIP as it checks the reviewed ones.
$shippedComponents = Join-Path $docDir 'legal/components.json'
if (!(Test-Path -LiteralPath $shippedComponents -PathType Leaf) -or
    (Get-NoticeHash -Path $shippedComponents) -ne (Get-NoticeHash -Path $componentsPath)) {
    throw "The legal component installed a components.json other than $componentsPath at $shippedComponents"
}
Assert-ReleaseNotices -DocDir $docDir
Assert-ReleaseBinaryRecords -TreeRoot $stage -ComponentsPath $shippedComponents
Write-NoticeManifest -TreeRoot $stage -ComponentsPath $shippedComponents
Assert-ReleaseNoticeContents -TreeRoot $stage -ComponentsPath $shippedComponents
Write-Host "Staged release tree: $stage"
