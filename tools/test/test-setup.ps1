<#
.SYNOPSIS
    Runs all WSL tests; optionally sets-up a WSL distribution and environment prior to running the tests.
.PARAMETER Version
    The version of WSL to run the tests in.
.PARAMETER DistroPath
    Path to a .tar/.tar.gz file of the distro to be imported to run the tests with, if specified.
.PARAMETER DistroName
    The name to be given to the imported distro.
.PARAMETER Package
    Path to the wsl.msix package to install. Optional.
.PARAMETER UnitTestsPath
    Path to the linux/unit_tests directory to copy and install the unit tests. Optional.
.PARAMETER PostInstallCommand
    Command to run post-installation and set-up of the WSL distro used for testing, if specified.
.PARAMETER AllowUnsigned
    Imports .\private-wsl.cert to the LocalMachine\Root store.
#>

[CmdletBinding()]
Param (
    [Parameter(Mandatory = $true)]$Version,
    $DistroPath,
    $DistroName,
    $Package = $null,
    $UnitTestsPath = $null,
    $PostInstallCommand = $null,
    [switch]$AllowUnsigned)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Run {
    [CmdletBinding()]
    param([ScriptBlock]$cmd)

    Invoke-Command -ScriptBlock $cmd
    if ($lastexitcode -ne 0) {
        throw ("$cmd failed with exit code: " + $lastexitcode)
    }
}

if ($Package) {
    $installedPackage = Get-AppxPackage MicrosoftCorporationII.WindowsSubsystemforLinux -AllUsers
    if ($installedPackage) {
        Write-Host "Removing previous package installation"
        Remove-AppxPackage $installedPackage -AllUsers
    }

    $installedMsi = (Get-ItemProperty -Path "HKLM:Software\Microsoft\Windows\CurrentVersion\Lxss\MSI" -Name ProductCode -ErrorAction Ignore)
    if ($installedMsi)
    {
        Write-Host "Removing MSI package: $($installedMsi.ProductCode)"
        $MSIArguments = @(
            "/norestart"
            "/qn"
            "/x"
            "$($installedMsi.ProductCode)"
            )

        $exitCode = (Start-Process -Wait "msiexec.exe" -ArgumentList $MSIArguments -NoNewWindow -PassThru).ExitCode
        if ($exitCode -Ne 0)
        {
            Write-Host "Failed to remove package: $exitCode"
            exit 1
        }

    }

    Write-Host "Installing package: $Package"
    try {
        if ($AllowUnsigned)
        {
            # unfortunately -AllowUnsigned isn't supported on vb so we need to manually import the certificate and trust it.
            (Get-AuthenticodeSignature $Package).SignerCertificate | Export-Certificate -FilePath private-wsl.cert | Out-Null
            try
            {
                Import-Certificate -FilePath .\private-wsl.cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
            }
            finally
            {
                Remove-Item -Path .\private-wsl.cert
            }
        }

        Add-AppxPackage $Package
    }
    catch {
        Write-Host $_
        Get-AppPackageLog -All
        exit 1
    }
}

# Disable OOBE during testing
$UserLxssRegistryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss"
If (-NOT (Test-Path $UserLxssRegistryPath))
{
    New-Item -Path $UserLxssRegistryPath -Force | Out-Null
}

New-ItemProperty -Path $UserLxssRegistryPath -Name "OOBEComplete" -Value "1" -PropertyType DWORD -Force | Out-Null

if ($DistroPath)
{
    & wsl.exe --unregister "$DistroName" # Ignore non-zero return for this call
    Run { wsl.exe --import "$DistroName" "$env:LocalAppData\lxss" "$DistroPath" --version "$Version" }
    Run { wsl.exe --set-default "$DistroName" }
}

if ($UnitTestsPath) {
    # get wslpath to unit tests
    $WslUnitTestPath = Run { wsl.exe --exec wslpath "$UnitTestsPath" }

    # set-up the folder structure for the tests and copy them into the linux distro
    Run { wsl.exe --exec bash -c "umask 0; mkdir -p /data;" }
    Run { wsl.exe --exec bash -c "umask 0; cp -r $WslUnitTestPath /data/test" }
    Run { wsl.exe --exec bash -c "umask 0; mkdir -p /data/test/log" }

    # ensure that /etc/fstab exists for the unit tests that expect it
    Run { wsl.exe --exec bash -c "( [ -e `"/etc/fstab`" ] || touch `"/etc/fstab`" )" }
}


if ($PostInstallCommand) {
    Run { wsl.exe "$PostInstallCommand" }
}
