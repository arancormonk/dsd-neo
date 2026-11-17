# SPDX-License-Identifier: GPL-3.0-or-later
# Helpers for tools/windows/verify_release_tree.ps1: an isolated process runner and
# readers for the text SoapySDRUtil, dsd-neo and dumpbin print. They take output as
# text, so tests can feed them saved output on any platform.

Set-StrictMode -Version Latest

# Messages SoapySDR prints when a module failed to load (Modules.in.cpp, Registry.cpp).
$script:SoapyLoadErrors = @('LoadLibrary() failed', 'failed ABI check', 'duplicate entry')

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

# The DLL names dumpbin /DEPENDENTS lists, delay-loaded ones included.
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
        } elseif ($inList -and $trimmed -match '^[A-Za-z0-9_.+-]+\.dll$') {
            $names.Add($trimmed)
        }
    }
    return $names.ToArray()
}

# Whether dumpbin /IMPORTS shows the function imported from the DLL by name. dumpbin
# prints each DLL's name on a line of its own, then its tables, then a "<hint> <name>"
# line per function.
function Test-DumpbinImport {
    param(
        [AllowEmptyString()]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Dll,

        [Parameter(Mandatory = $true)]
        [string]$Function
    )

    $current = $null
    foreach ($line in (ConvertTo-CheckLines $Text)) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^[A-Za-z0-9_.+-]+\.dll$') {
            $current = $trimmed
        } elseif ($trimmed -eq 'Summary') {
            $current = $null
        } elseif ($current -and $current -ieq $Dll -and $trimmed -match '^[0-9A-Fa-f]+\s+(\S+)$' -and
            $Matches[1] -ceq $Function) {
            return $true
        }
    }
    return $false
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
    Find-SoapyLoadError, Get-SoapyProbeMessages, Get-SoapyNoMatchMessage, Get-DumpbinDependents, Test-DumpbinImport,
    Invoke-IsolatedProcess
