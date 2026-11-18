# SPDX-License-Identifier: GPL-3.0-or-later
# Checks a Windows release tree, as stage_release_tree.ps1 leaves it or as the ZIP
# unpacks, without hardware:
#   0. every release notice is in share\doc\dsd-neo;
#   1. the bundled librtlsdr is the patched build;
#   2. the SoapySDR load path: one copy of each library, in bin\, each module and
#      library linked to what it should be (dsd-neo.exe's native Airspy and RTL-SDR
#      inputs and librtlsdr to the same airspy.dll and libusb-1.0.dll as the modules'
#      libraries), every file a binary of the tree imports, whatever its name ends in,
#      a file in bin\ unless it is an API set or one of the Windows DLLs
#      release_tree_checks.psm1 lists, no debug Visual C++ runtime, no Visual C++
#      runtime DLL that the msvc-runtime record the tree ships
#      (share\doc\dsd-neo\legal\components.json) does not list; the binaries are every
#      .exe and .dll in bin\ and the module folder and every file they import, directly
#      or not, and each is an x64 image that passes check_release_hardening.ps1;
#   3. every file in bin\ and lib\, hidden or not, is one a component record of the tree's
#      components.json lists (windows_release), and the tree carries the notices of
#      those records;
#   4. dsd-neo.exe -h;
#   5. SoapySDRUtil --check=<driver> finds each of the two drivers;
#   6. SoapySDRUtil --info finds exactly the two modules under this root;
#   7. dsd-neo.exe reaches each driver (it cannot open a device: there is none);
#   8. without lib\, no driver is found;
#   9. without bin\hackrf.dll, only the HackRF module fails to load.
# Each program runs in a fresh process from an empty working directory, with PATH
# reduced to bin\ and the Windows folders and SOAPY_SDR_ROOT, SOAPY_SDR_PLUGIN_PATH
# and DSD_NEO_CONFIG removed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Root,

    # dsd-neo.exe's exit code when it cannot open its input.
    [int]$ProbeExitCode = 1,

    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Import-Module (Join-Path $PSScriptRoot 'release_tree_checks.psm1') -Force

$Drivers = @('airspy', 'hackrf')
$Modules = @{ airspy = 'airspySupport.dll'; hackrf = 'HackRFSupport.dll' }
$Libraries = @{ airspy = 'airspy.dll'; hackrf = 'hackrf.dll' }
$ProbeMessages = Get-SoapyProbeMessages
$NoMatch = Get-SoapyNoMatchMessage

function Resolve-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw 'dumpbin.exe not found. Run from a Visual Studio developer shell.'
    }
    return $command.Source
}

function Invoke-Dumpbin {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Option,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $output = & $script:Dumpbin $Option $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin $Option failed for ${Path} (exit $LASTEXITCODE):`n$output"
    }
    return $output
}

# The DLLs an image imports. A report that is not about a loadable image (an object
# file, a library) would list none and pass every check below. An image of another
# machine than x64 passes them too, and the hardening check, but an x64 process cannot
# load it: a DLL the release delay-loads only when a receiver is opened would fail there,
# on the user's machine, not in the checks here.
function Get-ImageDependents {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $headers = Invoke-Dumpbin '/HEADERS' $Path
    try {
        $machine = Get-DumpbinMachine $headers
    } catch {
        throw "dumpbin's header report for ${Path}: $($_.Exception.Message)`n$headers"
    }
    Assert-ReleaseMachine -Machine $machine -Path $Path
    $report = Invoke-Dumpbin '/DEPENDENTS' $Path
    if (-not (Test-DumpbinImageReport $report)) {
        throw "dumpbin did not report ${Path} as an executable or a DLL:`n$report"
    }
    try {
        return @(Get-DumpbinDependents $report)
    } catch {
        throw "dumpbin's dependency report for ${Path}: $($_.Exception.Message)"
    }
}

function Get-IsolatedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Bin
    )

    $windows = $env:SystemRoot
    return "$Bin;$(Join-Path $windows 'System32');$windows"
}

# Runs a program from the tree and checks its exit code; returns the run.
function Invoke-TreeProgram {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Bin,

        [Parameter(Mandatory = $true)]
        [string]$Program,

        [AllowEmptyCollection()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [int]$ExpectedExitCode
    )

    $run = Invoke-IsolatedProcess -FilePath (Join-Path $Bin $Program) -Arguments $Arguments `
        -PathValue (Get-IsolatedPath $Bin) -TimeoutSeconds $TimeoutSeconds
    if ($run.ExitCode -ne $ExpectedExitCode) {
        $code = '{0} (0x{0:X8})' -f $run.ExitCode
        throw "$($run.Command) exited with $code, expected $ExpectedExitCode`n$($run.Output)"
    }
    Write-Host "OK: $($run.Command) (exit $($run.ExitCode))"
    return $run
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)]
        $Run,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    if (-not $Run.Output.Contains($Text)) {
        throw "$($Run.Command) did not print '$Text'`n$($Run.Output)"
    }
}

function Assert-Lacks {
    param(
        [Parameter(Mandatory = $true)]
        $Run,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    if ($Run.Output.Contains($Text)) {
        throw "$($Run.Command) printed '$Text'`n$($Run.Output)"
    }
}

function Assert-NoLoadError {
    param(
        [Parameter(Mandatory = $true)]
        $Run
    )

    $message = Find-SoapyLoadError $Run.Output
    if ($null -ne $message) {
        throw "$($Run.Command) reported a module load error ('$message')`n$($Run.Output)"
    }
}

function Get-ProbeArguments {
    param(
        [AllowEmptyString()]
        [string]$Driver
    )

    $source = 'soapy'
    if ($Driver) {
        $source = "soapy:driver=$Driver"
    }
    return @('--frontend', 'none', '-i', $source, '-o', 'null')
}

function Test-PatchedRtlsdr {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Bin
    )

    # The overlay applies the patches; this checks the DLL the tree ships came out of
    # that build. Only librtlsdr has a probe: the libairspy patch adds no mark to a
    # Windows build (tools/check_patched_usb_libs.sh probes it on Linux and macOS).
    $dlls = @(Get-ChildItem -Path $Bin -File -Force -Filter '*rtlsdr*.dll')
    if ($dlls.Count -ne 1) { throw "Expected exactly one staged librtlsdr DLL, found $($dlls.Count)" }
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dlls[0].FullName))
    if (-not $text.Contains('no transfer left, canceling')) {
        throw "$($dlls[0].Name) lacks the reader's 'no transfer left' message: it was not built with 0003-librtlsdr-account-for-every-transfer.patch"
    }
    Write-Host "OK: $($dlls[0].Name) is built with the librtlsdr transfer patches"
}

# Every binary Test-LoadPath found: each must pass check_release_hardening.ps1.
function Test-Hardening {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Images
    )

    if ($Images.Count -eq 0) { throw 'No PE files found in the tree' }
    foreach ($image in $Images) {
        & (Join-Path $PSScriptRoot '../check_release_hardening.ps1') $image
    }
}

function Test-LoadPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        # The Visual C++ runtime DLLs the tree's msvc-runtime record lists.
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.HashSet[string]]$RecordedRuntime
    )

    $bin = Join-Path $TreeRoot 'bin'
    $moduleDir = Join-Path $TreeRoot 'lib/SoapySDR/modules0.8'

    # Hidden files too: the ZIP carries them, and Windows loads them.
    foreach ($name in @('airspy.dll', 'hackrf.dll', 'libusb-1.0.dll', 'pthreadVC3.dll', 'rtlsdr.dll', 'SoapySDR.dll')) {
        $copies = @(Get-ChildItem -LiteralPath $TreeRoot -File -Recurse -Force -Filter $name)
        if ($copies.Count -ne 1 -or $copies[0].DirectoryName -ine $bin) {
            $paths = @($copies | ForEach-Object { $_.FullName })
            throw "Expected one $name, in $bin; found $($paths.Count): $($paths -join ', ')"
        }
    }
    $found = @(Get-ChildItem -Path $moduleDir -File -Force | Select-Object -ExpandProperty Name | Sort-Object)
    $expected = @($Modules.Values | Sort-Object)
    if (($found -join ',') -ne ($expected -join ',')) {
        throw "Expected exactly $($expected -join ', ') in $moduleDir; found $($found -join ', ')"
    }

    # By name, since Windows loads one file per name and the names are unique in the tree:
    # a second copy under another name (libairspy.dll beside airspy.dll) passes every
    # other check, but the native input would then drive the device through one copy and
    # the module through the other. The patched librtlsdr check above holds only for the
    # rtlsdr.dll dsd-neo.exe imports, and only with the libusb-1.0.dll the rest share.
    $links = [ordered]@{}
    $links[(Join-Path $bin 'dsd-neo.exe')] = @('airspy.dll', 'rtlsdr.dll', 'SoapySDR.dll')
    $links[(Join-Path $bin 'SoapySDRUtil.exe')] = @('SoapySDR.dll')
    $links[(Join-Path $bin 'rtlsdr.dll')] = @('libusb-1.0.dll')
    foreach ($driver in $Drivers) {
        $links[(Join-Path $moduleDir $Modules[$driver])] = @($Libraries[$driver], 'SoapySDR.dll')
        $links[(Join-Path $bin $Libraries[$driver])] = @('libusb-1.0.dll', 'pthreadVC3.dll')
    }
    foreach ($path in $links.Keys) {
        $dependents = @(Get-ImageDependents $path)
        foreach ($name in $links[$path]) {
            if ($dependents -notcontains $name) {
                throw "$path does not import $name; it imports $($dependents -join ', ')"
            }
        }
    }

    # Windows looks for a module's DLLs where it looks for the program's (bin\ first),
    # never in the module's own folder, so every file a binary of the tree imports must
    # be a file in bin\ unless it is part of Windows. The runner's System32 says nothing
    # about that: it holds the Visual C++ redistributable and whatever its driver
    # packages installed, which a user's may lack (Get-DllKind). Windows loads the file
    # an import names whatever its name ends in (vendor.drv), so the binaries are every
    # .exe and .dll in bin\ and the module folder and every file they import, directly
    # or not; dumpbin refuses any that is not an image, and Get-ImageDependents any that
    # is not x64.
    # A Visual C++ runtime DLL the tree carries, imported or not, must be one the
    # msvc-runtime record lists with Microsoft's terms for it.
    $missing = New-Object 'System.Collections.Generic.List[string]'
    $debug = New-Object 'System.Collections.Generic.List[string]'
    $unrecorded = New-Object 'System.Collections.Generic.List[string]'
    $images = New-Object 'System.Collections.Generic.List[string]'
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $queue = New-Object 'System.Collections.Generic.Queue[System.IO.FileInfo]'
    $roots = @(Get-ChildItem -Path $bin -File -Force) + @(Get-ChildItem -Path $moduleDir -File -Force)
    foreach ($root in @($roots | Where-Object { $_.Extension -in @('.exe', '.dll') })) {
        $queue.Enqueue($root)
    }
    while ($queue.Count -gt 0) {
        $binary = $queue.Dequeue()
        if (-not $seen.Add($binary.FullName)) {
            continue
        }
        $images.Add($binary.FullName)
        $ownKind = Get-DllKind -Name $binary.Name
        if ($ownKind -eq 'DebugRuntime') {
            $debug.Add($binary.Name)
        } elseif ($ownKind -eq 'Redistributable' -and -not $RecordedRuntime.Contains($binary.Name)) {
            $unrecorded.Add($binary.Name)
        }
        foreach ($name in @(Get-ImageDependents $binary.FullName)) {
            $kind = Get-DllKind -Name $name
            $path = Join-Path $bin $name
            if ($kind -eq 'DebugRuntime') {
                $debug.Add("$($binary.Name) -> $name")
            } elseif ($kind -eq 'Redistributable' -and -not $RecordedRuntime.Contains($name)) {
                $unrecorded.Add("$($binary.Name) -> $name")
            } elseif ($kind -eq 'ApiSet' -or $kind -eq 'Windows') {
                continue
            } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
                $queue.Enqueue((Get-Item -Force -LiteralPath $path))
            } else {
                $missing.Add("$($binary.Name) -> $name")
            }
        }
    }
    if ($debug.Count -gt 0) {
        throw "Staged binaries are or import the debug Visual C++ runtime, which may not be redistributed:`n$($debug -join "`n")"
    }
    if ($unrecorded.Count -gt 0) {
        throw "Staged binaries are or import Visual C++ runtime DLLs that the tree's msvc-runtime record does not list:`n$($unrecorded -join "`n")"
    }
    if ($missing.Count -gt 0) {
        throw "DLLs the staged binaries import are missing from ${bin}:`n$($missing -join "`n")"
    }

    Write-Host 'OK: the SoapySDR modules, their libraries and every DLL they import are in place'
    return , $images.ToArray()
}

function Test-Drivers {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot
    )

    $bin = Join-Path $TreeRoot 'bin'
    foreach ($driver in $Drivers) {
        $run = Invoke-TreeProgram -Bin $bin -Program 'SoapySDRUtil.exe' -Arguments @("--check=$driver") -ExpectedExitCode 0
        Assert-Contains $run "Checking driver '$driver'... PRESENT"
        Assert-NoLoadError $run
    }

    $run = Invoke-TreeProgram -Bin $bin -Program 'SoapySDRUtil.exe' -Arguments @('--info') -ExpectedExitCode 0
    Assert-NoLoadError $run
    $info = Get-SoapyUtilInfo $run.StdOut
    $rootPath = ConvertTo-ComparablePath $TreeRoot
    if ($null -eq $info.InstallRoot -or (ConvertTo-ComparablePath $info.InstallRoot) -ine $rootPath) {
        throw "SoapySDRUtil --info names install root '$($info.InstallRoot)', not $TreeRoot`n$($run.Output)"
    }
    $found = @($info.Modules | ForEach-Object { (ConvertTo-ComparablePath $_).ToLowerInvariant() } | Sort-Object)
    $expected = @($Modules.Values | ForEach-Object {
            (ConvertTo-ComparablePath (Join-Path $rootPath "lib/SoapySDR/modules0.8/$_")).ToLowerInvariant()
        } | Sort-Object)
    if (($found -join '|') -ne ($expected -join '|')) {
        throw "SoapySDRUtil --info found modules $($info.Modules -join ', ')`n$($run.Output)"
    }
    if ($null -eq $info.Factories -or ((@($info.Factories | Sort-Object) -join ', ') -cne ($Drivers -join ', '))) {
        throw "SoapySDRUtil --info lists drivers '$(@($info.Factories) -join ', ')', not $($Drivers -join ', ')`n$($run.Output)"
    }

    Invoke-TreeProgram -Bin $bin -Program 'dsd-neo.exe' -Arguments @('-h') -ExpectedExitCode 0 | Out-Null
    foreach ($driver in $ProbeMessages.Keys) {
        $run = Invoke-TreeProgram -Bin $bin -Program 'dsd-neo.exe' -Arguments (Get-ProbeArguments $driver) `
            -ExpectedExitCode $ProbeExitCode
        Assert-Contains $run $ProbeMessages[$driver]
        Assert-Lacks $run $NoMatch
        Assert-NoLoadError $run
    }
}

function Test-WithoutModules {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$Scratch
    )

    # A copy without lib\: the checks above must tell present from absent, and nothing
    # on the machine may supply a module.
    $copy = Join-Path $Scratch 'without-modules'
    New-Item -ItemType Directory -Force -Path $copy | Out-Null
    Copy-Item -Recurse -Force -LiteralPath (Join-Path $TreeRoot 'bin') -Destination $copy
    $bin = Join-Path $copy 'bin'
    foreach ($driver in $Drivers) {
        $run = Invoke-TreeProgram -Bin $bin -Program 'SoapySDRUtil.exe' -Arguments @("--check=$driver") -ExpectedExitCode 1
        Assert-Contains $run "Checking driver '$driver'... MISSING!"
    }
    foreach ($driver in $ProbeMessages.Keys) {
        $run = Invoke-TreeProgram -Bin $bin -Program 'dsd-neo.exe' -Arguments (Get-ProbeArguments $driver) `
            -ExpectedExitCode $ProbeExitCode
        Assert-Contains $run $NoMatch
    }
}

function Test-WithoutHackrfLibrary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$Scratch
    )

    # A copy without bin\hackrf.dll and nothing added to the module folder: SoapySDR
    # loads every DLL there by full path, so a DLL placed there would be loaded first or
    # not depending on its name. The HackRF module must then fail to load, the Airspy one not.
    $copy = Join-Path $Scratch 'without-hackrf'
    New-Item -ItemType Directory -Force -Path $copy | Out-Null
    Copy-Item -Recurse -Force -LiteralPath (Join-Path $TreeRoot 'bin') -Destination $copy
    Copy-Item -Recurse -Force -LiteralPath (Join-Path $TreeRoot 'lib') -Destination $copy
    Remove-Item -Force -LiteralPath (Join-Path $copy 'bin/hackrf.dll')
    $bin = Join-Path $copy 'bin'
    $run = Invoke-TreeProgram -Bin $bin -Program 'SoapySDRUtil.exe' -Arguments @('--check=hackrf') -ExpectedExitCode 1
    Assert-Contains $run "Checking driver 'hackrf'... MISSING!"
    Assert-Contains $run 'LoadLibrary() failed'
    $run = Invoke-TreeProgram -Bin $bin -Program 'SoapySDRUtil.exe' -Arguments @('--check=airspy') -ExpectedExitCode 0
    Assert-Contains $run "Checking driver 'airspy'... PRESENT"
}

$treeRoot = (Resolve-Path -LiteralPath $Root).ProviderPath
$script:Dumpbin = Resolve-Dumpbin
$tempBase = $env:RUNNER_TEMP
if (-not $tempBase) {
    $tempBase = [System.IO.Path]::GetTempPath()
}
$scratch = Join-Path $tempBase ('dsd-neo-verify-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
try {
    Assert-ReleaseNotices -DocDir (Join-Path $treeRoot 'share/doc/dsd-neo')
    Write-Host "OK: every release notice is in $(Join-Path $treeRoot 'share/doc/dsd-neo')"
    $recordedRuntime = Get-RecordedRuntimeFiles -ComponentsPath (Join-Path $treeRoot 'share/doc/dsd-neo/legal/components.json')
    Test-PatchedRtlsdr -Bin (Join-Path $treeRoot 'bin')
    $images = Test-LoadPath -TreeRoot $treeRoot -RecordedRuntime $recordedRuntime
    Assert-ReleaseBinaryRecords -TreeRoot $treeRoot -ComponentsPath (Join-Path $treeRoot 'share/doc/dsd-neo/legal/components.json')
    Write-Host 'OK: every file in bin\ and lib\ has a component record, and the tree carries its notices'
    Test-Hardening -Images $images
    Test-Drivers -TreeRoot $treeRoot
    Test-WithoutModules -TreeRoot $treeRoot -Scratch $scratch
    Test-WithoutHackrfLibrary -TreeRoot $treeRoot -Scratch $scratch
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Release tree verified: $treeRoot"
# GitHub's pwsh steps end with "exit $LASTEXITCODE"; the last program run above may
# have exited non-zero on purpose.
$global:LASTEXITCODE = 0
