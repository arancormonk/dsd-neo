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
$script:ReleaseNotices = @('LICENSE','COPYRIGHT','THIRD_PARTY.md','licenses/ezpwd-LGPL-2.1-or-later.txt','licenses/pffft-neo-README.md','licenses/pffft-neo-LICENSE.txt','licenses/pffft-LICENSE.txt','licenses/LicenseRef-PFFFT.txt','licenses/tetra_crypto-LICENSE.txt','licenses/libairspy-LICENSE.txt','licenses/librtlsdr-GPL-2.0-or-later.txt','licenses/librtlsdr-PATCHES.txt','legal/status.md','legal/components.json','licenses/ISC.txt','licenses/Unlicense.txt','licenses/gqrx-scanner-MIT.txt','licenses/LicenseRef-BCH3-Noncommercial.txt','licenses/LicenseRef-Simon-Rockliff.txt','licenses/mbelib-neo/ISC.txt','licenses/mbelib-neo/LicenseRef-ETSI-EN-300-395-2-data.txt','licenses/mbelib-neo/legal/status.md','licenses/mbelib-neo/legal/patents.md','licenses/BSL-1.0.txt','licenses/SoapyAirspy-MIT.txt','licenses/SoapyHackRF-MIT.txt','licenses/libhackrf-BSD-3-Clause.txt','licenses/libusb-LGPL-2.1-or-later.txt','licenses/PThreads4W-Apache-2.0.txt','licenses/PThreads4W-NOTICE.txt','licenses/SoapySDR-COPYRIGHT.txt','licenses/libusb-COPYRIGHT.txt','licenses/PThreads4W-CONTRIBUTORS.txt')

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
# a notice the legal CMake component installs), and for a notice from the repository
# the reviewed SHA-256 of its text (Reviewed: the record's notice_sha256 of that path,
# $null when it has none). Staging reads the repository's inventory, the release check
# the one the tree ships.
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
        $reviewed = @{}
        $hashes = $null
        if ($component.PSObject.Properties.Name -contains 'notice_sha256') {
            $hashes = $component.notice_sha256
        }
        foreach ($notice in @($component.windows_release.notices.PSObject.Properties)) {
            $notices[$notice.Name] = [string]$notice.Value
            if ($notices[$notice.Name] -match '^repo:(.+)$') {
                $source = $Matches[1]
                $reviewed[$notice.Name] = $null
                if ($null -ne $hashes -and $hashes.PSObject.Properties.Name -contains $source) {
                    $reviewed[$notice.Name] = [string]$hashes.$source
                }
            }
        }
        $record = [pscustomobject]@{ Component = [string]$component.id; Notices = $notices; Reviewed = $reviewed }
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

# Where a release tree records the SHA-256 of each notice staging took from somewhere
# other than the repository (the build tree, a vcpkg port, the legal CMake component,
# and the notices every tree carries that no record reviews), relative to
# share\doc\dsd-neo: one "<sha256>  <notice>" line each, as sha256sum prints them.
$script:NoticeManifest = 'notices.sha256'

function Get-NoticeManifestName {
    return $script:NoticeManifest
}

# The SHA-256 of a notice's text with CRLF line endings folded to LF, as
# tools/legal_inventory.py hashes a reviewed notice (notice_sha256), in lower-case hex.
function Get-NoticeHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $latin1 = [System.Text.Encoding]::Latin1
    $text = $latin1.GetString([System.IO.File]::ReadAllBytes($Path)).Replace("`r`n", "`n")
    $hash = [System.Security.Cryptography.SHA256]::HashData($latin1.GetBytes($text))
    return [System.Convert]::ToHexString($hash).ToLowerInvariant()
}

# The notices a release tree must carry under share\doc\dsd-neo, each with the reviewed
# SHA-256 of its text, or $null for a notice no record takes from the repository: the
# notices every tree carries (Get-ReleaseNotices) and those of the records of every file
# in bin\ and lib\. A notice from the repository whose record has no reviewed hash, or
# whose records disagree on it, throws.
function Get-ReleaseNoticeHashes {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    $records = Get-ReleaseBinaryRecords -ComponentsPath $ComponentsPath
    $reviewed = @{}
    foreach ($list in $records.Values) {
        foreach ($record in $list) {
            foreach ($notice in $record.Reviewed.Keys) {
                $hash = $record.Reviewed[$notice]
                if ([string]::IsNullOrEmpty($hash) -or $hash -notmatch '^[0-9a-f]{64}$') {
                    throw "The record $($record.Component) in $ComponentsPath takes $notice from the repository ($($record.Notices[$notice])) without a reviewed notice_sha256 for it"
                }
                if ($reviewed.ContainsKey($notice) -and $reviewed[$notice] -ne $hash) {
                    throw "The records in $ComponentsPath give $notice two reviewed hashes, $($reviewed[$notice]) and $hash"
                }
                $reviewed[$notice] = $hash
            }
        }
    }
    $required = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($notice in $script:ReleaseNotices) {
        [void]$required.Add($notice)
    }
    foreach ($file in @(Get-ReleaseTreeFiles -TreeRoot $TreeRoot)) {
        if ($records.ContainsKey($file.Name)) {
            foreach ($record in $records[$file.Name]) {
                foreach ($notice in $record.Notices.Keys) {
                    [void]$required.Add($notice)
                }
            }
        }
    }
    $result = [ordered]@{}
    foreach ($notice in $required) {
        $result[$notice] = if ($reviewed.ContainsKey($notice)) { $reviewed[$notice] } else { $null }
    }
    return $result
}

# Writes the tree's notice manifest (Get-NoticeManifestName): the SHA-256 of every notice
# the tree must carry that no record reviews (Get-ReleaseNoticeHashes), as staged.
function Write-NoticeManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    $docDir = Join-Path $TreeRoot 'share/doc/dsd-neo'
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $notices = Get-ReleaseNoticeHashes -TreeRoot $TreeRoot -ComponentsPath $ComponentsPath
    foreach ($notice in $notices.Keys) {
        if ($null -eq $notices[$notice]) {
            $lines.Add("$(Get-NoticeHash -Path (Join-Path $docDir $notice))  $notice")
        }
    }
    $text = ($lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText((Join-Path $docDir $script:NoticeManifest), $text, [System.Text.UTF8Encoding]::new($false))
}

# Throws unless every notice the tree must carry holds the text it should: a notice from
# the repository the text its record reviewed (notice_sha256 in the tree's
# components.json, CRLF folded to LF), any other the text staging recorded in the tree's
# notice manifest. A file that merely exists at a notice's path, such as a placeholder
# or the notice of another component, does not pass.
function Assert-ReleaseNoticeContents {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TreeRoot,

        [Parameter(Mandatory = $true)]
        [string]$ComponentsPath
    )

    $docDir = Join-Path $TreeRoot 'share/doc/dsd-neo'
    $manifestPath = Join-Path $docDir $script:NoticeManifest
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "No notice manifest at $manifestPath, so the notices staging took from the build, vcpkg and the legal component cannot be checked"
    }
    $recorded = @{}
    foreach ($line in (ConvertTo-CheckLines ([System.IO.File]::ReadAllText($manifestPath)))) {
        if ($line -eq '') {
            continue
        }
        if ($line -cnotmatch '^([0-9a-f]{64})  (\S.*)$' -or $recorded.ContainsKey($Matches[2])) {
            throw "$manifestPath has a line that is not one notice's SHA-256: '$line'"
        }
        $recorded[$Matches[2]] = $Matches[1]
    }
    $wrong = New-Object 'System.Collections.Generic.List[string]'
    $notices = Get-ReleaseNoticeHashes -TreeRoot $TreeRoot -ComponentsPath $ComponentsPath
    foreach ($notice in $notices.Keys) {
        $path = Join-Path $docDir $notice
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
            $wrong.Add("${path}: missing")
            continue
        }
        $actual = Get-NoticeHash -Path $path
        if ($null -ne $notices[$notice]) {
            if ($actual -ne $notices[$notice]) {
                $wrong.Add("${path}: SHA-256 $actual, not the reviewed $($notices[$notice]) (notice_sha256 in $ComponentsPath)")
            }
        } elseif (-not $recorded.ContainsKey($notice)) {
            $wrong.Add("${path}: no reviewed hash in $ComponentsPath and no line in $manifestPath")
        } elseif ($actual -ne $recorded[$notice]) {
            $wrong.Add("${path}: SHA-256 $actual, not the staged $($recorded[$notice]) ($manifestPath)")
        }
    }
    if ($wrong.Count -gt 0) {
        throw "Notices whose text is not the one the release must carry:`n$($wrong -join "`n")"
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

# The first warning or error dumpbin printed anywhere in a report, or $null. MSVC tools
# print a diagnostic as "<origin> : <severity> LNK<code>: <text>", and the origin is the
# tool (DUMPBIN, LINK) or the file it was reading, under the name it was given (name.dll,
# a full path); dumpbin still exits 0 after a warning. Any such line means it read the
# image only in part, wherever it stands in the report.
function Find-DumpbinDiagnostic {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    foreach ($line in (ConvertTo-CheckLines $Text)) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^(DUMPBIN|LINK)\s*:\s*(fatal error|error|warning)\b' -or
            $trimmed -match '(^|[\s:])(fatal error|error|warning)\s+LNK[0-9]{4}\s*:') {
            return $trimmed
        }
    }
    return $null
}

# The COFF machine type dumpbin /HEADERS reports in its file header values
# ("8664 machine (x64)"). Throws unless the report names exactly one, and when dumpbin
# printed a warning or an error (Find-DumpbinDiagnostic).
function Get-DumpbinMachine {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $diagnostic = Find-DumpbinDiagnostic $Text
    if ($null -ne $diagnostic) {
        throw "dumpbin printed '$diagnostic'"
    }
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
# and so does a warning or error dumpbin printed anywhere in the report
# (Find-DumpbinDiagnostic): it read the image only in part. A clean report without a
# list is an image that imports nothing.
function Get-DumpbinDependents {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    $diagnostic = Find-DumpbinDiagnostic $Text
    if ($null -ne $diagnostic) {
        throw "dumpbin printed '$diagnostic'"
    }
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

# GetFinalPathNameByHandleW: the path Windows opened, with 8.3 short names expanded and
# symbolic links, junctions and SUBST drives resolved. Compiled only on Windows, on first use.
$script:FinalPathSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class DsdNeoFinalPath
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security,
                                                     uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint size,
                                                         uint flags);

    public static string Get(string path)
    {
        // FILE_READ_ATTRIBUTES, any sharing, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS (a folder too).
        using (SafeFileHandle handle = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), path);
            }
            StringBuilder buffer = new StringBuilder(1024);
            uint length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
            if (length >= buffer.Capacity)
            {
                buffer = new StringBuilder((int)length + 1);
                length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
            }
            if (length == 0 || length >= buffer.Capacity)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), path);
            }
            string final = buffer.ToString(0, (int)length);
            if (final.StartsWith(@"\\?\UNC\", StringComparison.Ordinal))
            {
                return @"\\" + final.Substring(8);
            }
            return final.StartsWith(@"\\?\", StringComparison.Ordinal) ? final.Substring(4) : final;
        }
    }
}
'@

# The one path an existing file or folder has, whatever spelling named it: on Windows the
# path GetFinalPathNameByHandleW reports (short 8.3 names such as PROGRA~1 expanded,
# symbolic links and junctions resolved); elsewhere every symbolic link resolved, as
# realpath does. The paths come from a Windows build (its CMake cache, its developer
# shell), which may separate folders with either slash. Throws when nothing is there.
function Resolve-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $spelled = $Path.Trim()
    if (-not $IsWindows) {
        $spelled = $spelled.Replace('\', '/')
    }
    if (-not [System.IO.Path]::IsPathRooted($spelled)) {
        $spelled = Join-Path (Get-Location).ProviderPath $spelled
    }
    # .NET's own checks: the file system provider's Get-Item and Test-Path cost tens of
    # milliseconds each, and the walk below makes one check per folder.
    if (-not ([System.IO.File]::Exists($spelled) -or [System.IO.Directory]::Exists($spelled))) {
        throw "$Path does not exist on this machine"
    }
    if ($IsWindows) {
        if (-not ('DsdNeoFinalPath' -as [type])) {
            Add-Type -TypeDefinition $script:FinalPathSource
        }
        return [DsdNeoFinalPath]::Get($spelled).TrimEnd('\')
    }

    # Each part in turn from the root, a link replaced by its target and the walk
    # resumed there, so a '..' after a link climbs out of the link's target.
    $parts = [System.Collections.Generic.List[string]]::new()
    $parts.AddRange([string[]]@($spelled.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries)))
    $current = '/'
    $links = 0
    $index = 0
    while ($index -lt $parts.Count) {
        $part = $parts[$index]
        $index++
        if ($part -eq '.') {
            continue
        }
        if ($part -eq '..') {
            $parent = [System.IO.Path]::GetDirectoryName($current)
            $current = if ($null -eq $parent) { '/' } else { $parent }
            continue
        }
        $next = [System.IO.Path]::Join($current, $part)
        $item = [System.IO.FileInfo]::new($next)
        if (-not $item.LinkTarget -and -not ([System.IO.File]::Exists($next) -or [System.IO.Directory]::Exists($next))) {
            throw "$Path does not exist on this machine"
        }
        if ($item.LinkTarget) {
            $links++
            if ($links -gt 40) {
                throw "$Path has too many levels of symbolic links"
            }
            $target = [string]$item.LinkTarget
            $rest = $parts.GetRange($index, $parts.Count - $index)
            $parts = [System.Collections.Generic.List[string]]::new()
            $parts.AddRange([string[]]@($target.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries)))
            $parts.AddRange($rest)
            $index = 0
            if ($target.StartsWith('/')) {
                $current = '/'
            }
            continue
        }
        $current = $next
    }
    return $current
}

# Whether two canonical paths (Resolve-CanonicalPath) are the same: without regard to
# case on Windows, as its file systems compare names.
function Test-SamePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Other
    )

    $comparison = if ($IsWindows) { [System.StringComparison]::OrdinalIgnoreCase } else { [System.StringComparison]::Ordinal }
    return [string]::Equals($Path, $Other, $comparison)
}

# Whether a canonical path is the canonical folder itself or lies inside it: the folder's
# path followed by a separator, so C:\VC\Tools\MSVC\14.44 does not hold
# C:\VC\Tools\MSVC\14.44-old\cl.exe.
function Test-PathInside {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Folder
    )

    $separator = [System.IO.Path]::DirectorySeparatorChar
    $comparison = if ($IsWindows) { [System.StringComparison]::OrdinalIgnoreCase } else { [System.StringComparison]::Ordinal }
    $prefix = $Folder.TrimEnd($separator) + $separator
    return (Test-SamePath -Path $Path -Other $Folder) -or $Path.StartsWith($prefix, $comparison)
}

# The value of one entry of a build's CMakeCache.txt (NAME:TYPE=value). Throws unless the
# cache holds the entry exactly once.
function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Build,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $cache = Join-Path $Build 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cache -PathType Leaf)) {
        throw "No CMakeCache.txt in $Build, so the toolset and the vcpkg tree that built it are unknown"
    }
    $entries = @(Get-Content -LiteralPath $cache | Where-Object { $_ -cmatch "^$([regex]::Escape($Name)):[A-Z]+=" })
    if ($entries.Count -ne 1) {
        throw "$cache names $($entries.Count) $Name entries, not one"
    }
    return $entries[0].Substring($entries[0].IndexOf('=') + 1).Trim()
}

# The x64 Visual C++ runtime a developer shell names (VCToolsRedistDir), checked against
# the toolset it names (VCToolsInstallDir): both folders of one Visual Studio installation,
# <VC>\Tools\MSVC\<tools version> and <VC>\Redist\MSVC\<runtime version>, and a pair of
# that installation's version files in <VC>\Auxiliary\Build (or a folder in it), the ones
# the developer shell reads, Microsoft.VCToolsVersion.<name>.txt naming the tools
# version and Microsoft.VCRedistVersion.<name>.txt naming the runtime version. A runtime
# folder of another installation, or of another toolset of the same one, is refused: the
# release would carry and record a runtime its toolset did not come with. Returns the
# canonical runtime folder.
function Assert-ToolsetRedistributable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolsInstallDir,

        [Parameter(Mandatory = $true)]
        [string]$RedistDir
    )

    $tools = Resolve-CanonicalPath $ToolsInstallDir
    $redist = Resolve-CanonicalPath $RedistDir
    $toolsVc = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $tools))
    $redistVc = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $redist))
    $toolsShape = (Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $tools))) -eq 'Tools' -and
        (Split-Path -Leaf (Split-Path -Parent $tools)) -eq 'MSVC'
    $redistShape = (Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $redist))) -eq 'Redist' -and
        (Split-Path -Leaf (Split-Path -Parent $redist)) -eq 'MSVC'
    if (-not $toolsShape -or -not $redistShape) {
        throw "VCToolsInstallDir ($ToolsInstallDir) and VCToolsRedistDir ($RedistDir) are not a toolset's VC\Tools\MSVC\<version> and VC\Redist\MSVC\<version> folders"
    }
    if (-not (Test-SamePath -Path $toolsVc -Other $redistVc)) {
        throw "VCToolsRedistDir ($RedistDir) is not in the Visual Studio installation of VCToolsInstallDir ($ToolsInstallDir): the release would carry another installation's Visual C++ runtime"
    }
    $toolsVersion = Split-Path -Leaf $tools
    $redistVersion = Split-Path -Leaf $redist
    $build = Join-Path $toolsVc 'Auxiliary/Build'
    $folders = @()
    if ([System.IO.Directory]::Exists($build)) {
        $folders = @($build) + @([System.IO.Directory]::GetDirectories($build))
    }
    $paired = $false
    $named = $false
    foreach ($folder in $folders) {
        foreach ($file in @([System.IO.Directory]::GetFiles($folder, 'Microsoft.VCToolsVersion.*.txt'))) {
            if ([System.IO.File]::ReadAllText($file).Trim() -ne $toolsVersion) {
                continue
            }
            $named = $true
            $suffix = [System.IO.Path]::GetFileName($file).Substring('Microsoft.VCToolsVersion.'.Length)
            $pair = [System.IO.Path]::Join($folder, 'Microsoft.VCRedistVersion.' + $suffix)
            if ([System.IO.File]::Exists($pair) -and [System.IO.File]::ReadAllText($pair).Trim() -eq $redistVersion) {
                $paired = $true
            }
        }
    }
    if (-not $named) {
        throw "No Microsoft.VCToolsVersion.*.txt in $build names the toolset $toolsVersion of VCToolsInstallDir ($ToolsInstallDir), so its Visual C++ runtime is unknown"
    }
    if (-not $paired) {
        throw "VCToolsRedistDir ($RedistDir) is runtime $redistVersion, which no Microsoft.VCRedistVersion.*.txt in $build pairs with the toolset $toolsVersion of VCToolsInstallDir ($ToolsInstallDir): the release would carry another toolset's Visual C++ runtime"
    }
    return $redist
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
    Get-PeImageMachine, Test-PeImage, Assert-ReleaseMachine, Find-DumpbinDiagnostic, Get-DumpbinMachine,
    Test-DumpbinImageReport, Test-ImportName, Get-DumpbinDependents, Resolve-CanonicalPath, Test-SamePath,
    Test-PathInside, Get-CMakeCacheValue, Assert-ToolsetRedistributable, Get-NoticeManifestName, Get-NoticeHash,
    Get-ReleaseNoticeHashes, Write-NoticeManifest, Assert-ReleaseNoticeContents, Invoke-IsolatedProcess
