# SPDX-License-Identifier: GPL-3.0-or-later
# Helpers for the Windows release scripts in this folder: the release notices, a DLL
# classification, a PE image check, an isolated process runner and readers for the text
# SoapySDRUtil,
# dsd-neo and dumpbin print. The readers take output as text, so tests can feed them
# saved output on any platform.

Set-StrictMode -Version Latest

# Messages SoapySDR prints when a module failed to load (Modules.in.cpp, Registry.cpp).
$script:SoapyLoadErrors = @('LoadLibrary() failed', 'failed ABI check', 'duplicate entry')

# The notices every Windows release tree carries under share\doc\dsd-neo: the project's,
# the installed legal component's, the embedded and linked components', and those of
# the SoapySDR load path. stage_release_tree.ps1 stages them; verify_release_tree.ps1
# checks the staged tree and the unpacked ZIP for them.
$script:ReleaseNotices = @('LICENSE','COPYRIGHT','THIRD_PARTY.md','licenses/ezpwd-LGPL-2.1-or-later.txt','licenses/pffft-LICENSE.txt','licenses/pffft-NOTICES.txt','licenses/tetra_crypto-LICENSE.txt','licenses/libairspy-LICENSE.txt','licenses/librtlsdr-GPL-2.0-or-later.txt','licenses/librtlsdr-PATCHES.txt','legal/status.md','legal/components.json','licenses/ISC.txt','licenses/Unlicense.txt','licenses/gqrx-scanner-MIT.txt','licenses/LicenseRef-BCH3-Noncommercial.txt','licenses/LicenseRef-Simon-Rockliff.txt','licenses/mbelib-neo/ISC.txt','licenses/mbelib-neo/LicenseRef-ETSI-EN-300-395-2-data.txt','licenses/mbelib-neo/legal/status.md','licenses/mbelib-neo/legal/patents.md','licenses/BSL-1.0.txt','licenses/SoapyAirspy-MIT.txt','licenses/SoapyHackRF-MIT.txt','licenses/libhackrf-BSD-3-Clause.txt','licenses/libusb-LGPL-2.1-or-later.txt','licenses/PThreads4W-Apache-2.0.txt','licenses/PThreads4W-NOTICE.txt','licenses/SoapySDR-COPYRIGHT.txt','licenses/libusb-COPYRIGHT.txt','licenses/PThreads4W-CONTRIBUTORS.txt')

function Get-ReleaseNotices {
    return $script:ReleaseNotices
}

# Throws unless every release notice is a non-empty file under the documentation folder.
function Assert-ReleaseNotices {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DocDir
    )

    foreach ($notice in $script:ReleaseNotices) {
        $path = Join-Path $DocDir $notice
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -Force -LiteralPath $path).Length -eq 0) {
            throw "Missing or empty required license file: $path"
        }
    }
}

# What dsd-neo prints when it reaches a driver that finds no device, per driver ('' is
# a bare -i soapy): each module's exception with no device attached (SoapyAirspy and
# SoapyHackRF Settings.cpp), and SoapySDR's answer to a bare
# request when more than one driver is registered (Factory.cpp).
function Get-SoapyProbeMessages {
    return [ordered]@{
        airspy = 'Unable to open AirSpy device'
        hackrf = 'no hackrf device matches'
        '' = 'no driver specified and no enumeration results'
    }
}

# SoapySDR's answer when no registered driver matches the request.
function Get-SoapyNoMatchMessage {
    return 'SoapySDR::Device::make() no match'
}

# Text with CRLF and lone CR line endings turned into LF and terminal styling removed,
# so line checks never see a trailing CR.
function ConvertTo-CheckText {
    param(
        [AllowNull()]
        [AllowEmptyString()]
        [string]$Text
    )

    if ($null -eq $Text) {
        return ''
    }
    $plain = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    return [regex]::Replace($plain, '\x1b\[[0-9;?]*[A-Za-z]', '')
}

function ConvertTo-CheckLines {
    param(
        [AllowNull()]
        [AllowEmptyString()]
        [string]$Text
    )

    return @((ConvertTo-CheckText $Text).Split("`n"))
}

# A Windows path in one spelling: backslashes, no trailing separator. SoapySDR joins
# its root and module folder with forward slashes.
function ConvertTo-ComparablePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return $Path.Trim().Replace('/', '\').TrimEnd('\')
}

# What SoapySDRUtil --info reports: the install root, each module found (path only;
# a version or a load error may follow it) and the registered drivers.
function Get-SoapyUtilInfo {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $root = $null
    $modules = New-Object 'System.Collections.Generic.List[string]'
    $factories = $null
    foreach ($line in (ConvertTo-CheckLines $Text)) {
        if ($line -match '^Install root:\s*(.+?)\s*$') {
            $root = $Matches[1]
        } elseif ($line -match '^Module found:\s*(.+?\.dll)(\s+\(.*\))?\s*$') {
            $modules.Add($Matches[1])
        } elseif ($line -match '^Available factories\.\.\.\s*(.*?)\s*$') {
            $list = $Matches[1]
            if ($list -eq 'No factories found!' -or $list -eq '') {
                $factories = @()
            } else {
                $factories = @($list -split ',\s*')
            }
        }
    }
    return [pscustomobject]@{
        InstallRoot = $root
        Modules = $modules.ToArray()
        Factories = $factories
    }
}

# The first module load error SoapySDR reported in the text, or $null.
function Find-SoapyLoadError {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $plain = ConvertTo-CheckText $Text
    foreach ($message in $script:SoapyLoadErrors) {
        if ($plain.Contains($message)) {
            return $message
        }
    }
    return $null
}

# The Windows DLLs the release imports, each part of every Windows the release supports:
# what the import tables of the 2.12.1 ZIP and of later nightly builds name, besides API
# sets and the Visual C++ runtime. Only these stay out of bin\. Any other DLL, even one in
# the build machine's System32 (a driver package's libusbK.dll, say), is missing on a
# clean Windows, so the release carries it or staging fails; a new Windows DLL import is
# added here once it is known to be part of Windows.
$script:WindowsDlls = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]@('advapi32.dll', 'bcrypt.dll', 'crypt32.dll', 'gdi32.dll', 'iphlpapi.dll', 'kernel32.dll',
        'ole32.dll', 'secur32.dll', 'setupapi.dll', 'shlwapi.dll', 'user32.dll', 'winmm.dll', 'ws2_32.dll'),
    [System.StringComparer]::OrdinalIgnoreCase)

# How a release treats a DLL a binary imports, shared by the staging and the checks:
#   ApiSet           an API set name (api-ms-win-*, ext-ms-*), resolved by Windows itself;
#   Windows          a Windows DLL on the list above, never copied;
#   DebugRuntime     Microsoft's debug C and C++ runtime: ucrtbased.dll and the debug
#                    builds of the Visual C++ runtime DLLs below (vcruntime140d.dll,
#                    msvcp140d.dll, msvcp140d_atomic_wait.dll, vcruntime140_1d.dll,
#                    concrt140d.dll, mfc140ud.dll and the like), which a /MDd build imports
#                    and which may not be redistributed, so a release refuses it;
#   Redistributable  a Visual C++ runtime DLL (vcruntime140*.dll, msvcp140*.dll,
#                    concrt140, vccorlib140, vcomp140, vcamp140, mfc140*, and their
#                    Unicode and language builds), which comes with the Visual C++
#                    redistributable, never with Windows, so the release carries it in
#                    bin\ even when the build machine's System32 holds a copy, and only
#                    when the msvc-runtime record lists it (Get-RecordedRuntimeFiles);
#   Other            anything else: a library the release carries in bin\.
function Get-DllKind {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($Name -match '^(?i)(api-ms-win-|ext-ms-).+\.dll$') {
        return 'ApiSet'
    }
    if ($script:WindowsDlls.Contains($Name)) {
        return 'Windows'
    }
    if ($Name -match '^(?i)(ucrtbased|(vcruntime|msvcp|msvcr|concrt|vccorlib|vcomp|vcamp|mfcm?)[0-9]{3}u?(_[a-z0-9]+)?d(_[a-z0-9_]+)?)\.dll$') {
        return 'DebugRuntime'
    }
    if ($Name -match '^(?i)(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)[0-9]{3}[a-z]*(_[a-z0-9_]+)?\.dll$') {
        return 'Redistributable'
    }
    return 'Other'
}

# The Visual C++ runtime DLLs a release may carry: the windows_release binaries of the
# msvc-runtime record in legal/components.json, which also names Microsoft's terms for
# them. Staging reads the repository's record, the release check the one the tree
# ships, so neither carries a runtime DLL the record does not list. Throws when the file
# has no such record or the record lists no file.
function Get-RecordedRuntimeFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    if (!(Test-Path -LiteralPath $ComponentsPath -PathType Leaf)) {
        throw "No component inventory at $ComponentsPath, so the Visual C++ runtime DLLs a release may carry are unknown"
    }
    $records = @((Get-Content -Raw -LiteralPath $ComponentsPath | ConvertFrom-Json).components |
            Where-Object { $_.id -eq 'msvc-runtime' })
    if ($records.Count -ne 1 -or -not ($records[0].PSObject.Properties.Name -contains 'windows_release') -or
        -not ($records[0].windows_release.PSObject.Properties.Name -contains 'binaries')) {
        throw "$ComponentsPath has no msvc-runtime record listing the Visual C++ runtime DLLs a release may carry"
    }
    $files = @($records[0].windows_release.binaries | Where-Object { $_ -is [string] -and $_ -ne '' })
    if ($files.Count -eq 0) {
        throw "The msvc-runtime record in $ComponentsPath lists no Visual C++ runtime DLL"
    }
    $set = [System.Collections.Generic.HashSet[string]]::new([string[]]$files, [System.StringComparer]::OrdinalIgnoreCase)
    return , $set
}

# The component records of the files a Windows release carries: for each file name
# (any case), the records in legal/components.json whose windows_release lists it, each
# with its id and its notices: the path of each under share\doc\dsd-neo and where
# staging takes it from (repo:<path>, build:<path>, vcpkg:<port>, or legal-component for
# a notice the legal CMake component installs). Staging reads the repository's
# inventory, the release check the one the tree ships.
function Get-ReleaseBinaryRecords {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    if (!(Test-Path -LiteralPath $ComponentsPath -PathType Leaf)) {
        throw "No component inventory at $ComponentsPath, so the components of the release's files are unknown"
    }
    $map = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.List[object]]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($component in @((Get-Content -Raw -LiteralPath $ComponentsPath | ConvertFrom-Json).components)) {
        if (-not ($component.PSObject.Properties.Name -contains 'windows_release')) {
            continue
        }
        $notices = [ordered]@{}
        foreach ($notice in @($component.windows_release.notices.PSObject.Properties)) {
            $notices[$notice.Name] = [string]$notice.Value
        }
        $record = [pscustomobject]@{ Component = [string]$component.id; Notices = $notices }
        foreach ($name in @($component.windows_release.binaries)) {
            if (-not $map.ContainsKey($name)) {
                $map[$name] = New-Object 'System.Collections.Generic.List[object]'
            }
            $map[$name].Add($record)
        }
    }
    return , $map
}

# The files of a release tree that need a component record: every file in bin\ and
# under lib\, hidden ones included (the ZIP carries them), except the DLL list
# stage_runtime_dlls.ps1 writes (bin\dlls-manifest.txt).
function Get-ReleaseTreeFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot
    )

    $files = @()
    $bin = $null
    foreach ($folder in @('bin', 'lib')) {
        $path = Join-Path $TreeRoot $folder
        if (Test-Path -LiteralPath $path -PathType Container) {
            if ($folder -eq 'bin') {
                $bin = (Get-Item -Force -LiteralPath $path).FullName
            }
            $files += @(Get-ChildItem -LiteralPath $path -File -Recurse -Force)
        }
    }
    return @($files | Where-Object { -not ($_.Name -eq 'dlls-manifest.txt' -and $_.DirectoryName -eq $bin) } |
            Sort-Object -Property FullName)
}

# Throws unless every file in the tree's bin\ and lib\ has a component record in the
# inventory and every notice of those records is a non-empty file under
# share\doc\dsd-neo: a file no record lists ships without its notices.
function Assert-ReleaseBinaryRecords {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    $records = Get-ReleaseBinaryRecords -ComponentsPath $ComponentsPath
    $docDir = Join-Path $TreeRoot 'share/doc/dsd-neo'
    $unrecorded = New-Object 'System.Collections.Generic.List[string]'
    $missing = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in @(Get-ReleaseTreeFiles -TreeRoot $TreeRoot)) {
        if (-not $records.ContainsKey($file.Name)) {
            $unrecorded.Add($file.FullName)
            continue
        }
        foreach ($record in $records[$file.Name]) {
            foreach ($notice in $record.Notices.Keys) {
                $path = Join-Path $docDir $notice
                if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -Force -LiteralPath $path).Length -eq 0) {
                    $missing.Add("$($file.Name) ($($record.Component)): $path")
                }
            }
        }
    }
    if ($unrecorded.Count -gt 0) {
        throw "No component record in $ComponentsPath lists these files of the tree (windows_release), so their notices are unknown:`n$($unrecorded -join "`n")"
    }
    if ($missing.Count -gt 0) {
        throw "Missing or empty notices of the components of the tree's files:`n$(@($missing | Sort-Object -Unique) -join "`n")"
    }
}

# Fills $Buffer from the stream's position; $false when the file ends first.
function Read-StreamBytes {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Stream]$Stream,

        [Parameter(Mandatory = $true)]
        [byte[]]$Buffer
    )

    $filled = 0
    while ($filled -lt $Buffer.Length) {
        $count = $Stream.Read($Buffer, $filled, $Buffer.Length - $filled)
        if ($count -le 0) {
            return $false
        }
        $filled += $count
    }
    return $true
}

# The COFF machine type of the release's binaries: IMAGE_FILE_MACHINE_AMD64. Windows
# refuses to load an image of any other machine into an x64 process.
$script:ReleaseMachine = 0x8664

# The COFF machine type of a PE image Windows can load (an executable or a DLL), from its
# own headers: the MZ header, the PE signature its e_lfanew points to (anywhere in the
# file, past a DOS stub of any size), the image flag in the COFF header and a PE32 or
# PE32+ optional header. $null for any other file: an object file has none of these,
# though llvm-objdump names its format as it names an image's (coff-x86-64) and lists no
# imports for it.
function Get-PeImageMachine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $dos = New-Object byte[] 64
        if (-not (Read-StreamBytes -Stream $stream -Buffer $dos) -or $dos[0] -ne 0x4D -or $dos[1] -ne 0x5A) {
            return $null
        }
        # The PE signature, the 20-byte COFF header and the optional header's magic.
        $pe = [System.BitConverter]::ToInt32($dos, 0x3C)
        if ($pe -lt 64 -or [long]$pe + 26 -gt $stream.Length) {
            return $null
        }
        $stream.Position = $pe
        $header = New-Object byte[] 26
        if (-not (Read-StreamBytes -Stream $stream -Buffer $header)) {
            return $null
        }
    } finally {
        $stream.Dispose()
    }
    if ($header[0] -ne 0x50 -or $header[1] -ne 0x45 -or $header[2] -ne 0 -or $header[3] -ne 0) {
        return $null
    }
    $machine = [System.BitConverter]::ToUInt16($header, 4)
    $optionalHeaderSize = [System.BitConverter]::ToUInt16($header, 20)
    $characteristics = [System.BitConverter]::ToUInt16($header, 22)
    $magic = [System.BitConverter]::ToUInt16($header, 24)
    if (($optionalHeaderSize -ge 2) -and (($characteristics -band 0x0002) -ne 0) -and ($magic -eq 0x10B -or $magic -eq 0x20B)) {
        return [int]$machine
    }
    return $null
}

# Whether the file is a PE image Windows can load (Get-PeImageMachine).
function Test-PeImage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return $null -ne (Get-PeImageMachine -Path $Path)
}

# Throws unless the machine type is the release's (x64): an x64 process that loads a DLL
# of another machine, even only when a receiver is opened, fails with ERROR_BAD_EXE_FORMAT.
function Assert-ReleaseMachine {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Machine,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ($Machine -ne $script:ReleaseMachine) {
        throw ("{0} is built for COFF machine 0x{1:X4}, not x64 (0x{2:X4}): an x64 release cannot load it" -f $Path, $Machine, $script:ReleaseMachine)
    }
}

# The COFF machine type dumpbin /HEADERS reports in its file header values
# ("8664 machine (x64)"). Throws unless the report names exactly one.
function Get-DumpbinMachine {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $machines = @(ConvertTo-CheckLines $Text | Where-Object { $_ -match '^\s*([0-9A-Fa-f]{1,4}) machine( \(.*\))?\s*$' } |
            ForEach-Object { [System.Convert]::ToInt32(($_.Trim() -split '\s+')[0], 16) })
    if ($machines.Count -ne 1) {
        throw "dumpbin /HEADERS named $($machines.Count) machine types, not one"
    }
    return $machines[0]
}

# Whether dumpbin reported on a loadable image: its File Type is DLL or EXECUTABLE
# IMAGE (not COFF OBJECT or a library) and the report ends with a section summary,
# which it prints even for a DLL that imports nothing.
function Test-DumpbinImageReport {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $lines = ConvertTo-CheckLines $Text
    $image = @($lines | Where-Object { $_ -match '^\s*File Type:\s*(DLL|EXECUTABLE IMAGE)\s*$' })
    $summary = @($lines | Where-Object { $_ -match '^\s*Summary\s*$' })
    return ($image.Count -gt 0) -and ($summary.Count -gt 0)
}

# Whether a dependency an import table names is a file name Windows can load: not empty,
# no folder, none of the characters Windows refuses in a file name, and not ending in a
# dot or a space. Spaces inside and any extension are allowed (Vendor Support.dll,
# WINSPOOL.DRV).
function Test-ImportName {
    param(
        [AllowEmptyString()]
        [string]$Name
    )

    return ($Name -match '^[^<>:"/\\|?*\x00-\x1f]+$') -and ($Name -notmatch '[. ]$') -and ($Name -ne '..')
}

# The DLL names dumpbin /DEPENDENTS lists, delay-loaded ones included. Every line of a
# dependency list is one name; a line that is not throws, so no dependency goes unread,
# and so does a warning or error dumpbin printed anywhere in the report: it read the
# image only in part.
function Get-DumpbinDependents {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $names = New-Object 'System.Collections.Generic.List[string]'
    $inList = $false
    foreach ($line in (ConvertTo-CheckLines $Text)) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^(DUMPBIN|LINK)\s*:\s*(fatal error|error|warning)\b') {
            throw "dumpbin printed '$trimmed'"
        }
        if ($trimmed -match '^Image has the following (delay load )?dependencies:$') {
            $inList = $true
        } elseif ($trimmed -eq 'Summary') {
            $inList = $false
        } elseif ($inList -and $trimmed -ne '') {
            if (-not (Test-ImportName $trimmed)) {
                throw "dumpbin /DEPENDENTS listed '$trimmed', which is not a DLL name"
            }
            $names.Add($trimmed)
        }
    }
    return $names.ToArray()
}

# Runs a program in a fresh process, from a new empty working directory, with PATH
# set to $PathValue and the variables that would point SoapySDR or dsd-neo elsewhere
# removed. Returns the exit code and both streams as check text; throws when the
# program outlives the timeout.
function Invoke-IsolatedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [AllowEmptyCollection()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$PathValue,

        [int]$TimeoutSeconds = 120,

        [string[]]$RemoveVariables = @('SOAPY_SDR_ROOT', 'SOAPY_SDR_PLUGIN_PATH', 'DSD_NEO_CONFIG')
    )

    $command = (@($FilePath) + $Arguments) -join ' '
    $workDir = Join-Path ([System.IO.Path]::GetTempPath()) ('dsd-neo-run-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $workDir | Out-Null
    $process = $null
    try {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $FilePath
        foreach ($argument in $Arguments) {
            $info.ArgumentList.Add($argument)
        }
        $info.UseShellExecute = $false
        $info.RedirectStandardInput = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.WorkingDirectory = $workDir
        # Case-insensitive on Windows, so this replaces "Path" as well.
        $info.Environment['PATH'] = $PathValue
        foreach ($name in $RemoveVariables) {
            [void]$info.Environment.Remove($name)
        }

        $process = [System.Diagnostics.Process]::Start($info)
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "$command did not finish within $TimeoutSeconds s"
        }
        # The parameterless wait also waits for the redirected streams to drain.
        $process.WaitForExit()
        $out = ConvertTo-CheckText $stdout.GetAwaiter().GetResult()
        $err = ConvertTo-CheckText $stderr.GetAwaiter().GetResult()
        return [pscustomobject]@{
            Command = $command
            ExitCode = $process.ExitCode
            StdOut = $out
            StdErr = $err
            Output = $out + $err
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Export-ModuleMember -Function ConvertTo-CheckText, ConvertTo-CheckLines, ConvertTo-ComparablePath, Get-SoapyUtilInfo,
    Find-SoapyLoadError, Get-ReleaseNotices, Assert-ReleaseNotices, Get-ReleaseBinaryRecords, Get-ReleaseTreeFiles,
    Assert-ReleaseBinaryRecords, Get-SoapyProbeMessages, Get-SoapyNoMatchMessage, Get-DllKind, Get-RecordedRuntimeFiles,
    Get-PeImageMachine, Test-PeImage, Assert-ReleaseMachine, Get-DumpbinMachine, Test-DumpbinImageReport,
    Test-ImportName, Get-DumpbinDependents, Invoke-IsolatedProcess
