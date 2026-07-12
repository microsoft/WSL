<#
.SYNOPSIS
    Helper to pack WSL test data nuget.
.PARAMETER InputDirectory
    Directory containing arch-specific subdirectories (x64, arm64) with test data.
.PARAMETER Version
    Nuget package version.
.PARAMETER OutputDirectory
    Directory to place the packaged nuget file. Default to current working directory.
#>

[CmdletBinding(PositionalBinding=$False, DefaultParameterSetName='vm')]
param (
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$OutputDirectory = $PWD.Path
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not (Test-Path -Path $InputDirectory -PathType Container)) {
    throw("The path '$InputDirectory' is not an existing directory.")
}

$hasArch = (Test-Path "$InputDirectory\x64") -or (Test-Path "$InputDirectory\arm64")
if (-not $hasArch) {
    throw("The input directory must contain at least one architecture subdirectory (x64, arm64).")
}

echo "Building test data nuget. Input: $InputDirectory. Version: $Version"

Copy-Item -Path "$PSScriptRoot\Microsoft.WSL.TestData.nuspec" -Destination "$InputDirectory" -Force

& "$PSScriptRoot\..\..\_deps\nuget.exe" pack "$InputDirectory\Microsoft.WSL.TestData.nuspec" -Properties "version=$Version" -OutputDirectory "$OutputDirectory"