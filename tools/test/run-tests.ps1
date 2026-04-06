#Requires -RunAsAdministrator

<#
.SYNOPSIS
    Runs all WSL tests; optionally sets-up a WSL distribution and environment prior to running the tests.
.PARAMETER Version
    The version of WSL to run the tests in. Defaults to "2".
.PARAMETER SetupScript
    Path to a setup script to be run prior to running the tests. Defaults to ".\test-setup.ps1".
.PARAMETER DistroPath
    Path to a .tar/.tar.gz file of the distro to be imported to run the tests with. Defaults to ".\test_distro.tar.gz".
.PARAMETER Package
    Path to the wsl.msix package to install. Defaults to ".\wsl.msix".
.PARAMETER UnitTestsPath
    Path to the linux/unit_tests directory to copy and install the unit tests.
.PARAMETER PullRequest
    Switch for whether or not this test pass is being run as a part of a pull request; skips certain tests if present. Defaults to $false.
.PARAMETER TestDllPath
    Path to the TAEF test DLL. Defaults to ".\wsltests.dll".
.PARAMETER Fast
    Handy flag to skip package and distro installation to make tests run faster during development. 
.PARAMETER TeArgs
    Additional arguments for TE.exe.
#>

[cmdletbinding(PositionalBinding = $false)]
param (
    [string]$Version = 2,
    [string]$SetupScript = ".\test-setup.ps1",
    [string]$DistroPath = ".\test_distro.tar.gz",
    [string]$Package = ".\installer.msix",
    [string]$UnitTestsPath = ".\unit_tests",
    [switch]$PullRequest = $false,
    [string]$TestDllPath = ".\wsltests.dll",
    [switch]$Fast = $false,
    [parameter(ValueFromRemainingArguments = $true)]
    [string[]]$TeArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Fast)
{
    $SetupScript = $null
}

# Handle /attachdebugger: verify WinDbgX is available, then add /waitfordebugger so we can find and attach to the test host.
$AttachDebugger = $false
if ($TeArgs -and ($TeArgs -icontains '/attachdebugger'))
{
    $TeArgs = @($TeArgs | Where-Object { $_ -ine '/attachdebugger' })
    if (Get-Command "WinDbgX.exe" -ErrorAction SilentlyContinue)
    {
        $AttachDebugger = $true
        $TeArgs += '/waitfordebugger'
    }
    else
    {
        Write-Warning "/attachdebugger was requested, but WinDbgX.exe was not found. Continuing without debugger."
    }
}

$teArgList = @($TestDllPath, "/p:SetupScript=$SetupScript", "/p:Version=$Version", "/p:DistroPath=$DistroPath",
    "/p:Package=$Package", "/p:UnitTestsPath=$UnitTestsPath", "/p:PullRequest=$PullRequest", "/p:AllowUnsigned=1") + $TeArgs
$teProcess = Start-Process -FilePath "te.exe" -ArgumentList $teArgList -PassThru -NoNewWindow

if ($AttachDebugger)
{
    $targetPid = if ($TeArgs -icontains '/inproc') { $teProcess.Id } else { $null }

    if (-not $targetPid)
    {
        for ($i = 0; $i -lt 120 -and -not $teProcess.HasExited; $i++)
        {
            Start-Sleep -Milliseconds 500
            $child = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($teProcess.Id) AND Name = 'TE.ProcessHost.exe'" -ErrorAction SilentlyContinue
            if ($child) { $targetPid = $child[0].ProcessId; break }
        }
    }

    if ($targetPid)
    {
        $targetName = if ($targetPid -eq $teProcess.Id) { "TE.exe" } else { "TE.ProcessHost.exe" }
        Write-Host "Launching WinDbgX attached to $targetName (PID: $targetPid)..."
        Start-Process "WinDbgX.exe" -ArgumentList "-p $targetPid"
    }
    else
    {
        Write-Warning "Could not find TE.ProcessHost.exe within 60 seconds."
        Write-Host "Attach a debugger manually to TE.exe (PID: $($teProcess.Id))."
    }
}

$teProcess | Wait-Process
if ($teProcess.ExitCode -ne 0) { exit 1 }
