# OneFuzz VM setup for WSLC fuzzing.
# Installs Hyper-V and WSL so the service is available for SDK harnesses.
# RebootAfterSetup must be true in OneFuzzConfig.json for features to take effect.
#
# N.B. OneFuzz runs the setup script from a flat drop directory, so wsl.msi is staged
# next to this script there.

Set-Location -Path $PSScriptRoot
$ErrorActionPreference = "Stop"

Write-Host "Enabling Hyper-V..."
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -NoRestart

Write-Host "Enabling WSL..."
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux -NoRestart

$msiPath = Join-Path $PSScriptRoot "wsl.msi"
Write-Host "Installing WSL from $msiPath..."
Start-Process msiexec -ArgumentList "/i `"$msiPath`" /quiet /norestart" -Wait

Write-Host "Setup complete. Reboot required."
