<#
.SYNOPSIS
    Helper to pack WSL test data nuget.
.PARAMETER InputDirectory
    Directory containing the test data to be packaged.
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

echo "Building test data nuget. Input: $InputDirectory. Version: $Version"

Copy-Item -Path "$PSScriptRoot\Microsoft.WSL.TestData.nuspec" -Destination "$InputDirectory" -Force

& "$PSScriptRoot\..\..\_deps\nuget.exe" pack "$InputDirectory\Microsoft.WSL.TestData.nuspec" -Properties "version=$Version" -OutputDirectory "$OutputDirectory"