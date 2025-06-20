#Requires -RunAsAdministrator

# This script downloads and installs the latest version of the WSL MSI package

# Get current language code
$chcp_num = (chcp) -replace '[^\d]+(\d+).*','$1'

# Set language to english
chcp 437

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$release = Invoke-WebRequest 'https://api.github.com/repos/microsoft/WSL/releases/latest' | ConvertFrom-Json
$systeminfo = & systeminfo | findstr /C:"System Type"
if ($systeminfo -like '*x64*')
{
    $target = '.x64.msi'
} elseif ($systeminfo -like '*arm64*')
{
    $target = '.arm64.msi'
} else
{
    throw 'Failed to determine system type ($systeminfo)'
}

[array]$assets = $release.assets | Where-Object { $_.name.ToLower().endswith('.x64.msi')}
if ($assets.count -ne 1)
{
    throw 'Failed to find asset ($assets)'
}

$target = "$env:tmp\$($assets.name)"
Write-Host "Downloading $($assets.name) to $target"

$headers = New-Object "System.Collections.Generic.Dictionary[[String],[String]]"
$headers.Add('Accept','application/octet-stream')

Invoke-WebRequest $assets.url -Out $target -Headers $headers

$MSIArguments = @(
    "/i"
    $target
    "/qn"
    "/norestart"
)

$exitCode = (Start-Process -Wait "msiexec.exe" -ArgumentList $MSIArguments -NoNewWindow -PassThru).ExitCode
if ($exitCode -Ne 0)
{
    throw "Failed to install package: $exitCode"
}

Write-Host 'Installation complete'

Remove-Item $target -Force

# Restore original language
chcp $chcp_num
