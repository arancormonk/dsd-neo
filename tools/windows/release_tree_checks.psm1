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
$script:ReleaseNotices = @('LICENSE','COPYRIGHT','THIRD_PARTY.md','licenses/ezpwd-LGPL-2.1-or-later.txt','licenses/pffft-LICENSE.txt','licenses/pffft-NOTICES.txt','licenses/tetra_crypto-LICENSE.txt','licenses/libairspy-LICENSE.txt','licenses/librtlsdr-GPL-2.0-or-later.txt','licenses/librtlsdr-PATCHES.txt','legal/status.md','legal/components.json','licenses/ISC.txt','licenses/Unlicense.txt','licenses/gqrx-scanner-MIT.txt','licenses/LicenseRef-BCH3-Noncommercial.txt','licenses/LicenseRef-Simon-Rockliff.txt','licenses/mbelib-neo/ISC.txt','licenses/mbelib-neo/LicenseRef-ETSI-EN-300-395-2-data.txt','licenses/mbelib-neo/legal/status.md','licenses/mbelib-neo/legal/patents.md','licenses/BSL-1.0.txt','licenses/SoapyAirspy-MIT.txt','licenses/SoapyHackRF-MIT.txt','licenses/SoapyAirspyHF-MIT.txt','licenses/libhackrf-BSD-3-Clause.txt','licenses/libairspyhf-BSD-3-Clause.txt','licenses/libusb-LGPL-2.1-or-later.txt','licenses/PThreads4W-Apache-2.0.txt','licenses/PThreads4W-NOTICE.txt','licenses/SoapySDR-COPYRIGHT.txt','licenses/libusb-COPYRIGHT.txt','licenses/PThreads4W-CONTRIBUTORS.txt')

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
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
            throw "Missing or empty required license file: $path"
        }
    }
}

# What dsd-neo prints when it reaches a driver that finds no device, per driver ('' is
# a bare -i soapy): each module's exception with no device attached (SoapyAirspy,
# SoapyAirspyHF and SoapyHackRF Settings.cpp), and SoapySDR's answer to a bare
# request when more than one driver is registered (Factory.cpp).
function Get-SoapyProbeMessages {
    return [ordered]@{
        airspy = 'Unable to open AirSpy device'
        airspyhf = 'Unable to open AirspyHF device'
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
#                    bin\ even when the build machine's System32 holds a copy;
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

# Whether the file is a PE image Windows can load (an executable or a DLL), from its
# own headers: the MZ header, the PE signature its e_lfanew points to (anywhere in the
# file, past a DOS stub of any size), the image flag in the COFF header and a PE32 or
# PE32+ optional header. An object file has none of these, though llvm-objdump names its
# format as it names an image's (coff-x86-64) and lists no imports for it.
function Test-PeImage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $dos = New-Object byte[] 64
        if (-not (Read-StreamBytes -Stream $stream -Buffer $dos) -or $dos[0] -ne 0x4D -or $dos[1] -ne 0x5A) {
            return $false
        }
        # The PE signature, the 20-byte COFF header and the optional header's magic.
        $pe = [System.BitConverter]::ToInt32($dos, 0x3C)
        if ($pe -lt 64 -or [long]$pe + 26 -gt $stream.Length) {
            return $false
        }
        $stream.Position = $pe
        $header = New-Object byte[] 26
        if (-not (Read-StreamBytes -Stream $stream -Buffer $header)) {
            return $false
        }
    } finally {
        $stream.Dispose()
    }
    if ($header[0] -ne 0x50 -or $header[1] -ne 0x45 -or $header[2] -ne 0 -or $header[3] -ne 0) {
        return $false
    }
    $optionalHeaderSize = [System.BitConverter]::ToUInt16($header, 20)
    $characteristics = [System.BitConverter]::ToUInt16($header, 22)
    $magic = [System.BitConverter]::ToUInt16($header, 24)
    return ($optionalHeaderSize -ge 2) -and (($characteristics -band 0x0002) -ne 0) -and ($magic -eq 0x10B -or $magic -eq 0x20B)
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
# dependency list is one name; a line that is not throws, so no dependency goes unread.
function Get-DumpbinDependents {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $names = New-Object 'System.Collections.Generic.List[string]'
    $inList = $false
    foreach ($line in (ConvertTo-CheckLines $Text)) {
        $trimmed = $line.Trim()
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

# Whether dumpbin /IMPORTS shows the function imported from the DLL by name. dumpbin
# prints each DLL's name four spaces in, then its tables and a line per function, all
# further in: "<hint> <name>", "<address> <hint> <name>" for a delay-loaded one, or
# "Ordinal <n>". A line of an import section that is none of these throws, so a DLL
# name the reader did not recognise cannot lend its functions to the DLL before it.
function Test-DumpbinImport {
    param(
        [AllowEmptyString()]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Dll,

        [Parameter(Mandatory = $true)]
        [string]$Function
    )

    $tables = '^[0-9A-Fa-f]+ (Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference|Characteristics|Address of HMODULE|Bound Import Name Table|Unload Import Name Table)$'
    $found = $false
    $inSection = $false
    $current = $null
    foreach ($line in (ConvertTo-CheckLines $Text)) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^Section contains the following (delay load )?imports:$') {
            $inSection = $true
            $current = $null
        } elseif ($trimmed -eq 'Summary') {
            $inSection = $false
            $current = $null
        } elseif (-not $inSection -or $trimmed -eq '') {
            continue
        } elseif ($line -match '^ {4}\S') {
            if (-not (Test-ImportName $trimmed)) {
                throw "dumpbin /IMPORTS named '$trimmed' as a DLL, which is not a DLL name"
            }
            $current = $trimmed
        } elseif ($null -eq $current -or $line -notmatch '^ {5}') {
            throw "dumpbin /IMPORTS printed a line outside any DLL's imports: '$trimmed'"
        } elseif ($trimmed -match $tables -or $trimmed -match '^([0-9A-Fa-f]+\s+)?Ordinal\s+[0-9]+$') {
            continue
        } elseif ($trimmed -match '^([0-9A-Fa-f]+\s+)?[0-9A-Fa-f]+\s+(\S+)$') {
            if ($current -ieq $Dll -and $Matches[2] -ceq $Function) {
                $found = $true
            }
        } else {
            throw "dumpbin /IMPORTS printed a line it does not recognise under ${current}: '$trimmed'"
        }
    }
    return $found
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
    Find-SoapyLoadError, Get-ReleaseNotices, Assert-ReleaseNotices, Get-SoapyProbeMessages, Get-SoapyNoMatchMessage, Get-DllKind, Test-PeImage, Test-DumpbinImageReport,
    Test-ImportName, Get-DumpbinDependents, Test-DumpbinImport, Invoke-IsolatedProcess
