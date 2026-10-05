# OneFuzz VM setup for WSLC fuzzing.
# Installs WSL and Virtual Machine Platform so the service is available for SDK harnesses.
# RebootAfterSetup must be true in OneFuzzConfig.json for features to take effect.
#
# N.B. OneFuzz runs the setup script from a flat drop directory, so wsl.msi is staged
# next to this script there.

Set-Location -Path $PSScriptRoot
$ErrorActionPreference = "Stop"

Write-Host "Enabling WSL..."
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux -NoRestart

Write-Host "Enabling Virtual Machine Platform..."
Enable-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform -NoRestart

$msiPath = Join-Path $PSScriptRoot "wsl.msi"
if (!(Test-Path $msiPath))
{
    throw "WSL MSI not found: $msiPath"
}

Write-Host "Installing WSL from $msiPath..."
$installer = Start-Process msiexec -ArgumentList "/i `"$msiPath`" /quiet /norestart" -Wait -PassThru
if ($installer.ExitCode -notin @(0, 3010))
{
    throw "WSL MSI installation failed with exit code $($installer.ExitCode)"
}

Write-Host "Setup complete. Reboot required."
