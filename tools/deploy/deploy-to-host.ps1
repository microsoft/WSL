#Requires -RunAsAdministrator

[cmdletbinding(PositionalBinding = $false)]
param (
    [ValidateSet("Debug", "Release")][string]$BuildType = "Debug",
    [string]$BuildOutputPath = [string](Get-Location),
    [string]$PackageCertPath = $null,
    [parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MsiArgs
)

$ErrorActionPreference = "Stop"

$processorArchitecture = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
$Platform = switch -Wildcard ($processorArchitecture) {
    '*ARM64*' { 'arm64' }
    '*AMD64*' { 'X64' }
    default   { throw "Failed to determine system architecture: $processorArchitecture" }
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

$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($PackagePath, 0)
$view = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductCode'")
$view.Execute()
$record = $view.Fetch()
if ($null -eq $record)
{
    throw "ProductCode is missing from $PackagePath"
}

$packageProductCode = $record.StringData(1)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)

$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$installedProductKey = $registry.OpenSubKey("SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss\MSI")
$installedProductCode = if ($null -ne $installedProductKey) { $installedProductKey.GetValue("ProductCode") } else { $null }
if ($null -ne $installedProductKey)
{
    $installedProductKey.Dispose()
}
$registry.Dispose()

if ($packageProductCode -eq $installedProductCode)
{
    # Refresh all installed files and machine registry entries for the current product.
    $MSIArguments += @(
        "REINSTALL=ALL"
        "REINSTALLMODE=amus"
    )
}

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
