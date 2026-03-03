#Requires -RunAsAdministrator

[cmdletbinding(PositionalBinding = $false)]
param (
    [ValidateSet("X64", "arm64")][string]$Platform = $null,
    [ValidateSet("Debug", "Release")][string]$BuildType = "Debug",
    [string]$BuildOutputPath = [string](Get-Location),
    [string]$PackageCertPath = $null,
    [parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MsiArgs
)

$ErrorActionPreference = "Stop"

if (-not $Platform)
{
    $Platform = if ($env:PROCESSOR_ARCHITECTURE -ieq "ARM64") { "arm64" } else { "X64" }
}

$PackagePath = "$BuildOutputPath\bin\$Platform\$BuildType\wsl.msi"

# msiexec.exe doesn't like symlinks, so use the canonical path
$Target = (Get-ChildItem $PackagePath).Target
if ($Target)
{
    $PackagePath = $Target
}

Write-Host -ForegroundColor Green "Installing: $PackagePath "

$MSIArguments = @(
    "/i"
    $PackagePath
    "/qn"
    "/norestart"
)

if ($MsiArgs)
{
    $MSIArguments += $MsiArgs
}

$exitCode = (Start-Process -Wait "msiexec.exe" -ArgumentList $MSIArguments -NoNewWindow -PassThru).ExitCode
if ($exitCode -Ne 0)
{
    Write-Host "Failed to install package: $exitCode"
    exit 1
}

Write-Host -ForegroundColor Green "Package $PackagePath installed successfully"
