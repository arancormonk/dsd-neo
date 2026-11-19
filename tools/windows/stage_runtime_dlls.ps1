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

# Which objdump printed the report: 'pei' for GNU objdump, which calls a PE image pei-x86-64
# (an object file pe-x86-64), 'coff' for LLVM objdump, which calls both coff-x86-64 (the
# file's own headers tell those apart: Test-PeImage). $null unless the header is for the
# file that was asked about.
function Get-ObjdumpFormat {
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
            $candidate.Substring($expected.Length) -cmatch '^\s+file format (pei|coff)-x86-64\s*$') {
            return $Matches[1]
        }
    }
    return $null
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

# Whether objdump -p shows a non-empty entry of the image's data directory: entry 1, the
# import directory, or entry d, the delay import directory, which neither GNU nor LLVM
# objdump decodes (they list only the import table, so a DLL the image delay-loads goes
# unnamed). Throws when the report has no such entry to tell.
function Test-ObjdumpDirectoryEntry {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,

        [Parameter(Mandatory = $true)]
        [ValidateSet('1', 'd')]
        [string]$Index,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$BinaryPath
    )

    $name = @{ '1' = 'Import Directory'; 'd' = 'Delay Import Directory' }[$Index]
    $entries = @($Lines | Where-Object { $_ -cmatch "^Entry $Index\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+$name(\s|$)" })
    if ($entries.Count -ne 1) {
        throw "$Tool printed no $name entry for ${BinaryPath}, so the DLLs it loads are unknown:`n$($Lines -join "`n")"
    }
    $fields = $entries[0] -split '\s+'
    $address = [System.Convert]::ToUInt64($fields[2], 16)
    $size = [System.Convert]::ToUInt64($fields[3], 16)
    return ($address -ne 0) -or ($size -ne 0)
}

# Throws when an inspection tool printed a warning or an error: it read the image only
# in part, so its report may leave out a DLL the image loads. LLVM's tools put the
# severity after their own name; GNU objdump may put the file it read in between.
function Assert-NoToolDiagnostics {
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

    foreach ($line in $Lines) {
        if ($line -match '(objdump|readobj)(\.exe)?: (.+: )?(warning|error)\b') {
            throw "$Tool could not read all of ${BinaryPath}: '$line'"
        }
    }
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
            if ($line -cmatch '^(Import|DelayImport) \{$') {
                $block = $Matches[1]
                $name = $null
            } elseif ($line -notmatch '^((File|Format|Arch|AddressSize):\s.*)?\s*$') {
                throw "$Tool printed a line it does not recognise for ${BinaryPath}: '$line'"
            }
        } elseif ($line -ceq '}') {
            if ($null -eq $name) {
                throw "$Tool printed a $block block without a DLL name for ${BinaryPath}"
            }
            $names.Add($name)
            $block = $null
        } elseif ($line -notmatch '^  ') {
            throw "$Tool printed a line it does not recognise in a $block block for ${BinaryPath}: '$line'"
        } elseif ($line -cmatch '^  Name:(.*)$') {
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

# The DLL each entry of GNU objdump -p's import table names. Only the import table is
# read: the export table names the image itself. GNU objdump prints a row for each import
# descriptor, the all-zero one that ends the table included, and the DLL Name of each
# other descriptor after its row. It stops early, and still exits 0, when it cannot read
# a descriptor's DLL name (no DLL Name after its row), the descriptors (no row that ends
# the table) or the table itself ('There is an import table, but ...'): each of those
# throws, as does an image whose import directory it printed no table for. An image with
# no import directory has no table to print, and imports no DLL.
function Get-GnuObjdumpImportNames {
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

    $hasImports = Test-ObjdumpDirectoryEntry -Lines $Lines -Index '1' -Tool $Tool -BinaryPath $BinaryPath
    $names = New-Object 'System.Collections.Generic.List[string]'
    $state = 'before'
    $pending = $false
    foreach ($line in $Lines) {
        if ($state -eq 'before') {
            if ($line -cmatch '^There is an import table in \S+ at 0x[0-9a-f]+$') {
                $state = 'located'
            } elseif ($line -cmatch '^There is an import table\b') {
                throw "$Tool could not read the import table of ${BinaryPath}: '$line'"
            }
        } elseif ($state -eq 'located') {
            if ($line -cmatch '^The Import Tables \(interpreted \S+ section contents\)$') {
                $state = 'table'
            } elseif ($line -ne '') {
                throw "$Tool printed a line it does not recognise before the import table of ${BinaryPath}: '$line'"
            }
        } elseif ($state -eq 'table') {
            if ($line -cmatch '^ [0-9a-f]{8}\t([0-9a-f]{8}) [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} ([0-9a-f]{8})$') {
                if ($pending) {
                    throw "$Tool printed an import descriptor without a DLL name for ${BinaryPath}"
                }
                if ($Matches[1] -eq '00000000' -and $Matches[2] -eq '00000000') {
                    $state = 'done'
                } else {
                    $pending = $true
                }
            } elseif ($line -cmatch '^\tDLL Name:(.*)$') {
                $value = $Matches[1].Trim()
                if (-not $pending -or -not (Test-ImportName $value)) {
                    throw "$Tool printed '$($line.Trim())' for ${BinaryPath}, which names no DLL of an import descriptor"
                }
                $names.Add($value)
                $pending = $false
            } elseif ($line.Contains('<corrupt')) {
                throw "$Tool could not read the import table of ${BinaryPath}: '$($line.Trim())'"
            } elseif ($line -ne '' -and $line -cnotmatch '^\t' -and
                $line -cnotmatch '^ vma:\s+Hint\s+Time\s+Forward\s+DLL\s+First$' -and
                $line -cnotmatch '^\s+Table\s+Stamp\s+Chain\s+Name\s+Thunk$') {
                throw "$Tool printed a line it does not recognise in the import table of ${BinaryPath}: '$line'"
            }
        }
    }
    if ($state -eq 'located' -or $state -eq 'table') {
        throw "$Tool did not print the end of the import table of ${BinaryPath}, so the DLLs it loads are unknown"
    }
    if ($state -eq 'before' -and $hasImports) {
        throw "$Tool printed no import table for ${BinaryPath}, which has an import directory"
    }
    return $names.ToArray()
}

# The DLL each entry of LLVM objdump -p's import table names. Only the import table is
# read: the export table, which follows it, names the image itself ('DLL name:'). LLVM
# objdump prints the table only for an image that imports a DLL, and a row for each
# import descriptor before its DLL Name; a table without a row, or a row without a DLL
# Name, means it stopped early, which throws.
function Get-LlvmObjdumpImportNames {
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
    $state = 'before'
    $pending = $false
    $rows = 0
    foreach ($line in $Lines) {
        if ($state -eq 'before') {
            if ($line -ceq 'The Import Tables:') {
                $state = 'table'
            }
        } elseif ($state -eq 'table') {
            if ($line -cmatch '^  lookup [0-9a-f]{8} time [0-9a-f]{8} fwd [0-9a-f]{8} name [0-9a-f]{8} addr [0-9a-f]{8}$') {
                if ($pending) {
                    throw "$Tool printed an import descriptor without a DLL name for ${BinaryPath}"
                }
                $pending = $true
                $rows++
            } elseif ($line -cmatch '^    DLL Name:(.*)$') {
                $value = $Matches[1].Trim()
                if (-not $pending -or -not (Test-ImportName $value)) {
                    throw "$Tool printed '$($line.Trim())' for ${BinaryPath}, which names no DLL of an import descriptor"
                }
                $names.Add($value)
                $pending = $false
            } elseif ($line -ceq 'Export Table:') {
                if ($pending) {
                    throw "$Tool printed an import descriptor without a DLL name for ${BinaryPath}"
                }
                $state = 'done'
            } elseif ($line -ne '' -and $line -cnotmatch '^    ') {
                throw "$Tool printed a line it does not recognise in the import table of ${BinaryPath}: '$line'"
            }
        }
    }
    if ($pending) {
        throw "$Tool printed an import descriptor without a DLL name for ${BinaryPath}"
    }
    if ($state -ne 'before' -and $rows -eq 0) {
        throw "$Tool did not finish the import table of ${BinaryPath}, so the DLLs it loads are unknown"
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

    # Only an image has imports to read: an object file reports none and would pass. An
    # x64 process cannot load an image of another machine.
    $machine = Get-PeImageMachine -Path $BinaryPath
    if ($null -eq $machine) {
        throw "${BinaryPath} is not a PE image (an executable or a DLL)"
    }
    Assert-ReleaseMachine -Machine $machine -Path $BinaryPath

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
        Assert-NoToolDiagnostics -Lines $lines -Tool $tool -BinaryPath $BinaryPath
        if (-not (Test-ReadobjReport -Lines $lines -BinaryPath $BinaryPath)) {
            throw "$tool printed no COFF import report for ${BinaryPath}:`n$text"
        }
        $names = Get-ReadobjImportNames -Lines $lines -Tool $tool -BinaryPath $BinaryPath
    } else {
        Assert-NoToolDiagnostics -Lines $lines -Tool $tool -BinaryPath $BinaryPath
        $format = Get-ObjdumpFormat -Lines $lines -BinaryPath $BinaryPath
        if ($null -eq $format) {
            throw "$tool printed no PE report for ${BinaryPath}:`n$text"
        }
        if (Test-ObjdumpDirectoryEntry -Lines $lines -Index 'd' -Tool $tool -BinaryPath $BinaryPath) {
            throw "${BinaryPath} has delay-loaded DLLs, which $tool cannot list; run from a Visual Studio developer shell (dumpbin) or put llvm-readobj on PATH"
        }
        if ($format -eq 'pei') {
            $names = Get-GnuObjdumpImportNames -Lines $lines -Tool $tool -BinaryPath $BinaryPath
        } else {
            $names = Get-LlvmObjdumpImportNames -Lines $lines -Tool $tool -BinaryPath $BinaryPath
        }
    }

    # An empty list is valid: the report was read, and the binary imports no DLL.
    return $names | Sort-Object -Unique
}

# The x64 runtime folder of the Visual C++ toolset that built the binaries,
# VC\Redist\MSVC\<version>\x64\Microsoft.VC<toolset>.CRT, under VCToolsRedistDir, which
# the Visual Studio developer shell of that toolset sets (stage_release_tree.ps1 checks
# that the build's compilers are that shell's). Never another installed toolset's: an
# image built with one toolset may need a newer runtime than an older toolset's, and the
# release says which toolset its runtime came from. So VCToolsRedistDir must be the
# runtime folder the shell's own installation pairs with its toolset, VCToolsInstallDir
# (Assert-ToolsetRedistributable), or staging stops. Microsoft's REDIST list lets a
# licensed Visual Studio user distribute the files of VC\Redist, except its
# debug_nonredist folders (legal/components.json, msvc-runtime). $null when there is no
# such folder.
function Get-VcRuntimeDir {
    $redist = $env:VCToolsRedistDir
    if ([string]::IsNullOrWhiteSpace($redist)) {
        return $null
    }
    if ([string]::IsNullOrWhiteSpace($env:VCToolsInstallDir)) {
        throw "VCToolsRedistDir is set without VCToolsInstallDir, so the toolset its Visual C++ runtime must come with is unknown: run from the Visual Studio developer shell of the toolset that built the binaries"
    }
    $redist = Assert-ToolsetRedistributable -ToolsInstallDir $env:VCToolsInstallDir -RedistDir $redist
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

# Throws unless a Visual C++ runtime DLL already in the destination is the toolset's own
# copy, byte for byte: a destination file is kept rather than copied again, and the
# release may carry only the runtime of the toolset that built it.
function Assert-RuntimeCopy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [AllowNull()]
        [AllowEmptyString()]
        [string]$RuntimeDir
    )

    if (-not $RuntimeDir) {
        throw "$Path is already in the destination, and with no Visual C++ runtime folder to compare it with it may come from another toolset: run from the Visual Studio developer shell of the toolset that built the binaries, which sets VCToolsRedistDir"
    }
    $source = Join-Path $RuntimeDir $Name
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "$Path is already in the destination, but $Name is not in the Visual C++ runtime folder $RuntimeDir"
    }
    $kept = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    $toolset = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
    if ($kept -ne $toolset) {
        throw "$Path differs from ${source}: a Visual C++ runtime DLL in the release must be the copy of the toolset that built it"
    }
}

if (!(Test-Path -LiteralPath $TargetBinary)) {
    throw "TargetBinary not found: $TargetBinary"
}

New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null

$destinationFull = (Resolve-Path -LiteralPath $DestinationDir).Path
$targetFull = (Resolve-Path -LiteralPath $TargetBinary).Path

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
            if ($kind -eq 'Redistributable') {
                Assert-RuntimeCopy -Path $destDllPath -Name $dll -RuntimeDir $runtimeDir
            }
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
                $missing.Add("$dll (imported by $current; no Visual C++ runtime folder: run from the Visual Studio developer shell of the toolset that built it, which sets VCToolsRedistDir)")
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
Get-ChildItem -LiteralPath $destinationFull -Filter '*.dll' -File -Force -ErrorAction SilentlyContinue |
    Sort-Object -Property Name |
    Select-Object -ExpandProperty Name |
    Set-Content -LiteralPath $manifest -Encoding UTF8
