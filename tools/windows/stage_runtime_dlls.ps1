# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetBinary,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDir,

    [Parameter(Mandatory = $true)]
    [string[]]$SearchDirs,

    # The component inventory whose msvc-runtime record lists the Visual C++ runtime DLLs
    # a release may carry.
    [string]$ComponentsPath = (Join-Path (Join-Path (Join-Path $PSScriptRoot '..') '..') 'legal/components.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# The inspection tools' exit codes are checked below, where the failure can name the binary.
$PSNativeCommandUseErrorActionPreference = $false
Import-Module (Join-Path $PSScriptRoot 'release_tree_checks.psm1') -Force

function Get-VsInstallationPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $vswhere)) {
        return $null
    }

    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    if ([string]::IsNullOrWhiteSpace($path)) {
        return $null
    }
    return $path.Trim()
}

function Get-DumpbinPath {
    $cmd = Get-Command 'dumpbin' -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Path
    }

    $vsPath = Get-VsInstallationPath
    if (-not $vsPath) {
        return $null
    }

    $vcToolsRoot = Join-Path $vsPath 'VC\Tools\MSVC'
    if (!(Test-Path $vcToolsRoot)) {
        return $null
    }

    $vcTools = Get-ChildItem -Path $vcToolsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object -Property Name -Descending |
        Select-Object -First 1
    if (-not $vcTools) {
        return $null
    }

    $dumpbin = Join-Path $vcTools.FullName 'bin\Hostx64\x64\dumpbin.exe'
    if (Test-Path $dumpbin) {
        return $dumpbin
    }

    return $null
}

function Get-ReadobjPath {
    $cmd = Get-Command 'llvm-readobj' -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Path
    }

    return $null
}

function Get-ObjdumpPath {
    $cmd = Get-Command 'llvm-objdump' -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Path
    }

    $cmd = Get-Command 'objdump' -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Path
    }

    return $null
}

# GNU objdump calls a PE image pei-x86-64 (an object file pe-x86-64), LLVM objdump calls both coff-x86-64; the
# file's own headers tell those apart (Test-PeImage). The header must be for the file that was asked about.
function Test-ObjdumpReport {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $expected = $BinaryPath.Replace('\', '/') + ':'
    foreach ($line in $Lines) {
        $candidate = $line.Replace('\', '/')
        if ($candidate.StartsWith($expected, [System.StringComparison]::OrdinalIgnoreCase) -and
            $candidate.Substring($expected.Length) -match '^\s+file format (pei|coff)-x86-64\s*$') {
            return $true
        }
    }
    return $false
}

# llvm-readobj --coff-imports starts with the file it read and its format.
function Test-ReadobjReport {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $expected = $BinaryPath.Replace('\', '/')
    $file = @($Lines | Where-Object { $_ -match '^File:\s*(.+?)\s*$' -and $Matches[1].Replace('\', '/') -ieq $expected })
    $format = @($Lines | Where-Object { $_ -match '^Format:\s*COFF-x86-64\s*$' })
    return ($file.Count -eq 1) -and ($format.Count -eq 1)
}

# Whether objdump -p shows a delay import directory, which neither GNU nor LLVM objdump
# decodes: they list only the import table, so a DLL the image delay-loads goes unnamed.
# Throws when the report has no data directory to tell.
function Test-ObjdumpDelayImports {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $entries = @($Lines | Where-Object { $_ -match '^Entry d\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+Delay Import Directory\s*$' })
    if ($entries.Count -ne 1) {
        throw "$Tool printed no Delay Import Directory entry for ${BinaryPath}, so its delay-loaded DLLs are unknown:`n$($Lines -join "`n")"
    }
    $fields = $entries[0] -split '\s+'
    $address = [System.Convert]::ToUInt64($fields[2], 16)
    $size = [System.Convert]::ToUInt64($fields[3], 16)
    return ($address -ne 0) -or ($size -ne 0)
}

# The DLL each Import and DelayImport block of llvm-readobj --coff-imports names in its
# Name line. A block without exactly one Name that is a DLL name, or a line outside the
# blocks other than the report's header, throws: a dependency must never go unread.
function Get-ReadobjImportNames {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $names = New-Object 'System.Collections.Generic.List[string]'
    $block = $null
    $name = $null
    foreach ($line in $Lines) {
        if ($null -eq $block) {
            if ($line -match '^(Import|DelayImport) \{$') {
                $block = $Matches[1]
                $name = $null
            } elseif ($line -notmatch '^((File|Format|Arch|AddressSize):\s.*)?\s*$') {
                throw "$Tool printed a line it does not recognise for ${BinaryPath}: '$line'"
            }
        } elseif ($line -eq '}') {
            if ($null -eq $name) {
                throw "$Tool printed a $block block without a DLL name for ${BinaryPath}"
            }
            $names.Add($name)
            $block = $null
        } elseif ($line -match '^  Name:(.*)$') {
            $value = $Matches[1].Trim()
            if ($null -ne $name -or -not (Test-ImportName $value)) {
                throw "$Tool printed '$($line.Trim())' in a $block block for ${BinaryPath}, which is not one DLL name"
            }
            $name = $value
        }
    }
    if ($null -ne $block) {
        throw "$Tool printed an unfinished $block block for ${BinaryPath}"
    }
    return $names.ToArray()
}

# The DLL each DLL Name line of objdump -p names. A DLL Name line without a DLL name
# throws.
function Get-ObjdumpImportNames {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $names = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in $Lines) {
        if ($line -match 'DLL Name:') {
            if ($line -notmatch '^\s*DLL Name:(.*)$' -or -not (Test-ImportName $Matches[1].Trim())) {
                throw "$Tool printed '$($line.Trim())' for ${BinaryPath}, which names no DLL"
            }
            $names.Add($Matches[1].Trim())
        }
    }
    return $names.ToArray()
}

# The DLLs an image imports, those it delay-loads included: dumpbin /DEPENDENTS and
# llvm-readobj --coff-imports list both; objdump -p lists only the import table, so it
# is used only for an image without delay imports.
function Get-DependentDllNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryPath,

        [Parameter(Mandatory = $false)]
        [string]$DumpbinPath,

        [Parameter(Mandatory = $false)]
        [string]$ReadobjPath,

        [Parameter(Mandatory = $false)]
        [string]$ObjdumpPath
    )

    # Only an image has imports to read: an object file reports none and would pass.
    if (-not (Test-PeImage -Path $BinaryPath)) {
        throw "${BinaryPath} is not a PE image (an executable or a DLL)"
    }

    if ($DumpbinPath) {
        $tool = 'dumpbin'
        $output = & $DumpbinPath /DEPENDENTS $BinaryPath 2>&1
    } elseif ($ReadobjPath) {
        $tool = [System.IO.Path]::GetFileNameWithoutExtension($ReadobjPath)
        $output = & $ReadobjPath --coff-imports $BinaryPath 2>&1
    } elseif ($ObjdumpPath) {
        $tool = [System.IO.Path]::GetFileNameWithoutExtension($ObjdumpPath)
        $output = & $ObjdumpPath -p $BinaryPath 2>&1
    } else {
        throw 'None of dumpbin, llvm-readobj and objdump could be found; cannot resolve DLL dependencies.'
    }
    $exitCode = $LASTEXITCODE

    # Both streams, one line each; the tools end lines with CRLF on Windows.
    $lines = @($output | ForEach-Object { "$_".TrimEnd("`r") })
    $text = $lines -join "`n"
    if ($exitCode -ne 0) {
        throw "$tool failed for ${BinaryPath} (exit $exitCode):`n$text"
    }

    if ($DumpbinPath) {
        if (-not (Test-DumpbinImageReport -Text $text)) {
            throw "dumpbin printed no dependency report of an image for ${BinaryPath}:`n$text"
        }
        try {
            $names = Get-DumpbinDependents -Text $text
        } catch {
            throw "dumpbin's dependency report for ${BinaryPath}: $($_.Exception.Message)"
        }
    } elseif ($ReadobjPath) {
        if (-not (Test-ReadobjReport -Lines $lines -BinaryPath $BinaryPath)) {
            throw "$tool printed no COFF import report for ${BinaryPath}:`n$text"
        }
        $names = Get-ReadobjImportNames -Lines $lines -Tool $tool -BinaryPath $BinaryPath
    } else {
        if (-not (Test-ObjdumpReport -Lines $lines -BinaryPath $BinaryPath)) {
            throw "$tool printed no PE report for ${BinaryPath}:`n$text"
        }
        if (Test-ObjdumpDelayImports -Lines $lines -Tool $tool -BinaryPath $BinaryPath) {
            throw "${BinaryPath} has delay-loaded DLLs, which $tool cannot list; run from a Visual Studio developer shell (dumpbin) or put llvm-readobj on PATH"
        }
        $names = Get-ObjdumpImportNames -Lines $lines -Tool $tool -BinaryPath $BinaryPath
    }

    # An empty list is valid: the report was read, and the binary imports no DLL.
    return $names | Sort-Object -Unique
}

# The x64 runtime folder of the Visual C++ toolset that built the binaries,
# VC\Redist\MSVC\<version>\x64\Microsoft.VC<toolset>.CRT: under VCToolsRedistDir, which a
# Visual Studio developer shell sets for its toolset, else under the newest toolset of
# the latest Visual Studio. Microsoft's REDIST list lets a licensed Visual Studio user
# distribute the files of VC\Redist, except its debug_nonredist folders (legal/components.json,
# msvc-runtime). $null when there is no such folder.
function Get-VcRuntimeDir {
    $redist = $env:VCToolsRedistDir
    if ([string]::IsNullOrWhiteSpace($redist)) {
        $vsPath = Get-VsInstallationPath
        if (-not $vsPath) {
            return $null
        }
        $redistRoot = Join-Path $vsPath 'VC\Redist\MSVC'
        if (!(Test-Path -LiteralPath $redistRoot)) {
            return $null
        }
        $newest = Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^[0-9]+\.[0-9]+\.[0-9]+$' } |
            Sort-Object -Property { [version]$_.Name } -Descending |
            Select-Object -First 1
        if (-not $newest) {
            return $null
        }
        $redist = $newest.FullName
    }
    $arch = Join-Path $redist 'x64'
    if (!(Test-Path -LiteralPath $arch -PathType Container)) {
        return $null
    }
    $crt = @(Get-ChildItem -LiteralPath $arch -Directory | Where-Object { $_.Name -match '^Microsoft\.VC[0-9]+\.CRT$' })
    if ($crt.Count -gt 1) {
        throw "More than one Visual C++ runtime folder in ${arch}: $(@($crt | ForEach-Object { $_.Name }) -join ', ')"
    }
    if ($crt.Count -eq 0) {
        return $null
    }
    return $crt[0].FullName
}

if (!(Test-Path $TargetBinary)) {
    throw "TargetBinary not found: $TargetBinary"
}

New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null

$destinationFull = (Resolve-Path $DestinationDir).Path
$targetFull = (Resolve-Path $TargetBinary).Path

$dumpbin = Get-DumpbinPath
$readobj = Get-ReadobjPath
$objdump = Get-ObjdumpPath

$ComponentsPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ComponentsPath)
$recordedRuntime = Get-RecordedRuntimeFiles -ComponentsPath $ComponentsPath
$runtimeDir = Get-VcRuntimeDir

function Get-NormalizedDirs {
    param(
        [AllowEmptyCollection()]
        [string[]]$Dirs
    )

    return @($Dirs |
        Where-Object { $_ -and -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_.Trim().TrimEnd('\', '/') } |
        Select-Object -Unique)
}

# A library comes from the search folders only. A Visual C++ runtime DLL comes only from
# the toolset's runtime folder (Get-VcRuntimeDir), never from the build machine's
# System32 or a search folder, and only when the msvc-runtime record lists it: a clean
# Windows lacks it, so the release carries it (Get-DllKind). A function that returns one
# folder returns a string, not an array, so its result is put back into one before a list
# is added to it.
$libraryDirs = @(Get-NormalizedDirs (@($destinationFull) + $SearchDirs))

$queue = New-Object 'System.Collections.Generic.Queue[string]'
$visited = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$missing = New-Object 'System.Collections.Generic.List[string]'

$queue.Enqueue($targetFull)

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    if ($visited.Contains($current)) {
        continue
    }
    $visited.Add($current) | Out-Null

    $deps = Get-DependentDllNames -BinaryPath $current -DumpbinPath $dumpbin -ReadobjPath $readobj -ObjdumpPath $objdump
    foreach ($dll in $deps) {
        $kind = Get-DllKind -Name $dll
        if ($kind -eq 'ApiSet' -or $kind -eq 'Windows') {
            continue
        }
        if ($kind -eq 'DebugRuntime') {
            throw "$current imports $dll, part of the debug Visual C++ runtime, which may not be redistributed: build it with the release runtime (/MD)"
        }

        if ($kind -eq 'Redistributable' -and -not $recordedRuntime.Contains($dll)) {
            throw "$current imports $dll, a Visual C++ runtime DLL that the msvc-runtime record in $ComponentsPath does not list: record it there, with Microsoft's terms for it, before a release carries it"
        }

        $destDllPath = Join-Path $destinationFull $dll
        if (Test-Path -LiteralPath $destDllPath) {
            $queue.Enqueue((Resolve-Path -LiteralPath $destDllPath).Path)
            continue
        }

        $dirs = $libraryDirs
        if ($kind -eq 'Redistributable') {
            $dirs = @($runtimeDir | Where-Object { $_ })
        }
        $sourcePath = $null
        foreach ($dir in $dirs) {
            $candidate = Join-Path $dir $dll
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $sourcePath = (Resolve-Path -LiteralPath $candidate).Path
                break
            }
        }

        if (-not $sourcePath) {
            if ($kind -ne 'Redistributable') {
                $missing.Add("$dll (imported by $current)")
            } elseif ($runtimeDir) {
                $missing.Add("$dll (imported by $current; not in the Visual C++ runtime folder $runtimeDir)")
            } else {
                $missing.Add("$dll (imported by $current; no Visual C++ runtime folder: run from a Visual Studio developer shell, or install the toolset's redistributable files)")
            }
            continue
        }

        Copy-Item -Force -LiteralPath $sourcePath -Destination $destinationFull
        $queue.Enqueue((Join-Path $destinationFull $dll))
    }
}

if ($missing.Count -gt 0) {
    $unique = $missing | Sort-Object -Unique
    throw ("Missing DLL dependencies (not in the search folders; only the Windows DLLs release_tree_checks.psm1 lists may stay out of the release):`n" + ($unique -join "`n"))
}

$manifest = Join-Path $destinationFull 'dlls-manifest.txt'
Get-ChildItem -Path $destinationFull -Filter '*.dll' -File -ErrorAction SilentlyContinue |
    Sort-Object -Property Name |
    Select-Object -ExpandProperty Name |
    Set-Content -Path $manifest -Encoding UTF8
