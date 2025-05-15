<#
.SYNOPSIS
    Copies linux unit tests to a given WSL distribution.
.PARAMETER WslTestDirPath
    The absolute path to the location of the \wsl-build\test\linux directory.
.PARAMETER DistroName
    The name of the WSL distribution to copy the unit tests inside of.
    Defaults to "test_distro"
#>

param (
    [Parameter( ValueFromPipeline = $true,
        ValueFromPipelineByPropertyName = $true,
        ParameterSetName = "WslTestDirPath",
        HelpMessage = "Path to \wsl-build\test\linux\ location")]
    [Parameter( ParameterSetName = "CopyTestAll ")]
    [Alias('testpath')]
    [string[]]$WslTestDirPath,
    [Parameter( ValueFromPipeline = $true,
        ValueFromPipelineByPropertyName = $true,
        Mandatory = $false,
        ParameterSetName = "DistroName",
        HelpMessage = "Name of WSL distro to run tests in; defaults to test_distro")]
    [Parameter( ParameterSetName = "CopyTestAll ")]
    [Alias('distro')]
    [string[]]$DistroName = 'test_distro'
)

Write-Output "WslTestDirPath: $WslTestDirPath"
Write-Output "DistroName: $DistroName"

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Run {
    [CmdletBinding()]
    param([ScriptBlock]$cmd)

    Invoke-Command -ScriptBlock $cmd
    if ($LastExitCode -ne 0) {
        throw ("$cmd failed with exit code: " + $lastexitcode)
    }
}

$DistroPath = "$env:LocalAppData\lxss"

Write-Output "Copying unit tests to $DistroPath\rootfs\data\test"

# get wslpath to unit tests
$WslUnitTestPath = Run { wsl.exe --exec wslpath "$WslTestDirPath\unit_tests" }

# set-up the folder structure for the tests and copy them into the linux distro
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "umask 0; mkdir -p /data;" }
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "umask 0; cp -r $WslUnitTestPath /data/test" }
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "umask 0; mkdir -p /data/test/log; ls -la /data/test" }

# ensure that /etc/fstab exists for the unit tests that expect it
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "( [ -e `"/etc/fstab`" ] || touch `"/etc/fstab`" )" }