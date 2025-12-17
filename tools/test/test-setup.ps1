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
            # Try to add with -AllowUnsigned first (supported in newer PowerShell)
            try {
                Add-AppxPackage $Package -AllowUnsigned -ErrorAction Stop
            }
            catch {
                # Fallback: manually import the certificate and trust it
                Write-Host "Attempting to import package certificate..."
                $signature = Get-AuthenticodeSignature -LiteralPath $Package
                if (-not $signature.SignerCertificate) {
                    Write-Error "Package is not signed or has no certificate. Cannot import certificate."
                    exit 1
                }

                $cert = $signature.SignerCertificate
                $certPath = Join-Path $env:TEMP "wsl-package-cert.cer"
                try {
                    $cert | Export-Certificate -FilePath $certPath | Out-Null
                    Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
                    Write-Host "Certificate imported successfully. Retrying package installation..."
                }
                finally {
                    Remove-Item -Path $certPath -ErrorAction SilentlyContinue
                }

                # Retry installation after importing certificate
                Add-AppxPackage $Package -ErrorAction Stop
            }
        }
        else {
            Add-AppxPackage $Package -ErrorAction Stop
        }
    }
    catch {
        Write-Host "Error installing package: $_"
        Get-AppPackageLog | Select-Object -First 64 | Format-List
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
