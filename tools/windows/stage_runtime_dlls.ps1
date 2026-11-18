# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetBinary,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDir,

    [Parameter(Mandatory = $true)]
    [string[]]$SearchDirs
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

function Get-DependentDllNames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryPath,

        [Parameter(Mandatory = $false)]
        [string]$DumpbinPath,

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
    } elseif ($ObjdumpPath) {
        $tool = [System.IO.Path]::GetFileNameWithoutExtension($ObjdumpPath)
        $output = & $ObjdumpPath -p $BinaryPath 2>&1
    } else {
        throw 'Neither dumpbin nor objdump could be found; cannot resolve DLL dependencies.'
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
        $names = $lines |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -match '^[A-Za-z0-9_.+-]+\.dll$' }
    } else {
        if (-not (Test-ObjdumpReport -Lines $lines -BinaryPath $BinaryPath)) {
            throw "$tool printed no PE report for ${BinaryPath}:`n$text"
        }
        $names = $lines |
            ForEach-Object {
                if ($_ -match 'DLL Name:\s*(.+\.dll)\s*$') { $Matches[1] }
            } |
            Where-Object { $_ }
    }

    # An empty list is valid: the report was read, and the binary imports no DLL.
    return $names | Sort-Object -Unique
}

function Get-VsRedistDirs {
    $vsPath = Get-VsInstallationPath
    if (-not $vsPath) {
        return @()
    }

    $redistRoot = Join-Path $vsPath 'VC\Redist\MSVC'
    if (!(Test-Path $redistRoot)) {
        return @()
    }

    $dirs = @()
    $redist = Get-ChildItem -Path $redistRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object -Property Name -Descending |
        Select-Object -First 3
    foreach ($r in $redist) {
        foreach ($cand in @(
            (Join-Path $r.FullName 'x64\Microsoft.VC143.CRT'),
            (Join-Path $r.FullName 'x64\Microsoft.VC142.CRT'),
            (Join-Path $r.FullName 'x64\Microsoft.VC141.CRT')
        )) {
            if (Test-Path $cand) {
                $dirs += $cand
            }
        }
    }
    return $dirs | Select-Object -Unique
}

if (!(Test-Path $TargetBinary)) {
    throw "TargetBinary not found: $TargetBinary"
}

New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null

$destinationFull = (Resolve-Path $DestinationDir).Path
$targetFull = (Resolve-Path $TargetBinary).Path

$dumpbin = Get-DumpbinPath
$objdump = Get-ObjdumpPath

$system32 = Join-Path $env:SystemRoot 'System32'
if (Test-Path $system32) {
    $system32 = (Resolve-Path $system32).Path
}

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

# A library comes from the search folders only. A Visual C++ runtime DLL may also come
# from Visual Studio's redistributable folders or the build machine's System32: a clean
# Windows lacks it, so it is copied from wherever it is found (Get-DllKind).
$libraryDirs = Get-NormalizedDirs (@($destinationFull) + $SearchDirs)
$runtimeDirs = Get-NormalizedDirs ($libraryDirs + @(Get-VsRedistDirs) + @($system32))

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

    $deps = Get-DependentDllNames -BinaryPath $current -DumpbinPath $dumpbin -ObjdumpPath $objdump
    foreach ($dll in $deps) {
        $kind = Get-DllKind -Name $dll
        if ($kind -eq 'ApiSet' -or $kind -eq 'Windows') {
            continue
        }
        if ($kind -eq 'DebugRuntime') {
            throw "$current imports $dll, part of the debug Visual C++ runtime, which may not be redistributed: build it with the release runtime (/MD)"
        }

        $destDllPath = Join-Path $destinationFull $dll
        if (Test-Path $destDllPath) {
            $queue.Enqueue((Resolve-Path $destDllPath).Path)
            continue
        }

        $dirs = $libraryDirs
        if ($kind -eq 'Redistributable') {
            $dirs = $runtimeDirs
        }
        $sourcePath = $null
        foreach ($dir in $dirs) {
            $candidate = Join-Path $dir $dll
            if (Test-Path $candidate) {
                $sourcePath = (Resolve-Path $candidate).Path
                break
            }
        }

        if (-not $sourcePath) {
            $missing.Add("$dll (imported by $current)")
            continue
        }

        Copy-Item -Force $sourcePath $destinationFull
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
