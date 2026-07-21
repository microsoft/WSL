<#
.SYNOPSIS
    Copies linux unit tests to a given WSL distribution and builds them.
.PARAMETER WslTestDirPath
    The absolute path to the location of the \wsl-build\test\linux directory.
.PARAMETER DistroName
    The name of the WSL distribution to copy and build the tests inside of.
    Defaults to "test_distro"
#>

param (
    [Parameter( ValueFromPipeline = $true,
        ValueFromPipelineByPropertyName = $true,
        Mandatory = $true,
        ParameterSetName = "WslTestDirPath",
        HelpMessage = "Path to \wsl-build\test\linux\ location")]
    [Parameter( ParameterSetName = "CopyBuildTestAll ")]
    [Alias('testpath')]
    [string[]]$WslTestDirPath,
    [Parameter( ValueFromPipeline = $true,
        ValueFromPipelineByPropertyName = $true,
        Mandatory = $false,
        ParameterSetName = "DistroName",
        HelpMessage = "Name of WSL distro to run tests in; defaults to test_distro")]
    [Parameter( ParameterSetName = "CopyBuildTestAll ")]
    [Alias('distro')]
    [string[]]$DistroName = "test_distro"
)

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

$copyScriptCommand = $PSScriptRoot + "\copy_tests.ps1 -WslTestDirPath $WslTestDirPath -DistroName $DistroName"

$cleanTestCommand = "rm -rf /data/test"
$buildTestCommand = "cd /data/test; ./build_tests.sh; less /data/test/log/build_output"

# clean test directory on linux side
Write-Output "Cleaning unit tests at $DistroPath\rootfs\data\test"
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "$cleanTestCommand" }

# call the logic in copy_tests.ps1
Invoke-Expression $copyScriptCommand

# build the tests on the linux side
Write-Output "Building unit tests at $DistroPath\rootfs\data\test\"
Run { wsl.exe --distribution $DistroName --user root --exec bash -c "$buildTestCommand" }