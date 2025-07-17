<#
.SYNOPSIS
    Sets up a given Machine for running WSL tests.
.PARAMETER VMName
    Name of the VM or Computer to set-up for testing.
.PARAMETER ComputerName 
    Name of the Computer to set-up for testing.
.PARAMETER IPv6Addr 
    IPv6 address of the computer to set-up for testing.
.PARAMETER Credential
    A credential object to be used for authenticating with New-PSSession.
.PARAMETER Username
    Username on the given VM to set-up the tests under.
.PARAMETER Password
    Password associated with the Username, if needed.
.PARAMETER ArtifactFolder
    Specify when using cmake -S ArtifactFolder.
.PARAMETER BuildType
    The type of build to create when compiling the WSL source code. Defaults to "Debug".
.PARAMETER MakeTrusted
    An optional switch to add the destination to the TrustedHosts list.
.PARAMETER SkipEnableFeatures
    Skip enabling optional Windows features necessary for some tests.
.PARAMETER RemoteFolder
    Absolute Path to a folder on the VM to copy requisite files to. Defaults to "C:\Package".
.PARAMETER TaefFolder
    Absolute Path to a folder on the VM to copy taef binaries. Defaults to "C:\Taef".
.PARAMETER SkipDistro
    Skip copying over the distro.
.PARAMETER TestDistroPath
    Path to the distro image to import and use for testing, if needed. Auto filled if left empty.
#>

[CmdletBinding(PositionalBinding=$False, DefaultParameterSetName='vm')]
param (
    [Parameter(Mandatory = $true, ParameterSetName='vm')][string]$VMName,
    [Parameter(Mandatory = $true, ParameterSetName='hostname')][string]$ComputerName,
    [Parameter(Mandatory = $true, ParameterSetName='ipv6')][string]$IPv6Addr,
    [System.Management.Automation.PSCredential]$Credential,
    [string]$Username,
    [string]$Password,
    [string]$ArtifactFolder,
    [ValidateSet("Debug", "Release")][string]$BuildType = "Debug",
    [switch]$MakeTrusted,
    [switch]$SkipEnableFeatures,
    [string]$RemoteFolder = "C:\Package",
    [string]$TaefFolder = "C:\Taef",
    [switch]$SkipDistro,
    [string]$TestDistroPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($Credential -eq $null) {
    if ([string]::IsNullOrEmpty($Username)) {
        $Credential = Get-Credential
    }
    else {
        if ([string]::IsNullOrEmpty($Password)) {
            $SecurePassword = New-Object System.Security.SecureString
        }
        else {
            $SecurePassword = ConvertTo-SecureString "$Password" -AsPlainText -Force
        }
        $Credential = New-Object System.Management.Automation.PSCredential("$Username", $SecurePassword)
    }
}

if ($PSCmdlet.ParameterSetName -eq 'ipv6') {
    $ComputerName = $Ipv6Addr.Replace(":", "-")+".ipv6-literal.net"
}

if (![string]::IsNullOrEmpty($ComputerName) -and $MakeTrusted) {
    Set-Item -Path WSMan:\localhost\Client\TrustedHosts -Concatenate -Value $ComputerName
}

$Session = switch ($PSCmdlet.ParameterSetName) {
    "vm" {
        New-PSSession -VMName $VMName -Credential $Credential
    }
    default {
        New-PSSession `
            -Authentication Negotiate `
            -ComputerName $ComputerName `
            -Credential $Credential
    }
}
$Arch = Invoke-Command -Session $Session {$env:PROCESSOR_ARCHITECTURE}
$Platform = switch ($Arch) {
    "AMD64" {"X64"}
    "ARM64" {"arm64"}
    default { throw "Architecture $Arch unknown" }
}

if ([string]::IsNullOrEmpty($TestDistroPath)) {
    $TestDistroVersion = (Select-Xml -Path "$PSScriptRoot\..\..\packages.config" -XPath '/packages/package[@id=''Microsoft.WSL.TestDistro'']/@version').Node.Value
    $TestDistroPath =  "$PSScriptRoot\..\..\packages\Microsoft.WSL.TestDistro.$TestDistroVersion\test_distro.tar.xz"
}

if ([string]::IsNullOrEmpty($ArtifactFolder)) {
    $ArtifactFolder = "$PSScriptRoot/../.."
}

$Bin = "$((Get-ItemProperty -Path $ArtifactFolder).FullName)/bin/$Platform/$BuildType"

if (!$SkipEnableFeatures) {
    $Reboot = Invoke-Command -Session $Session -ScriptBlock {
        $RestartNeeded = $false

        $OptionalFeatureNames = @(
            "VirtualMachinePlatform", # Needed to run V2 tests
            "Microsoft-Hyper-V-Management-PowerShell", # Needed for mount tests
            "Microsoft-Windows-Subsystem-Linux") # Needed to run V1 tests

        foreach ($Name in $OptionalFeatureNames) {
            if ((Get-WindowsOptionalFeature -Online -FeatureName $Name).State -ne "Enabled") {
                $RestartNeeded = (Enable-WindowsOptionalFeature -Online -All -NoRestart -FeatureName $Name -WarningAction SilentlyContinue).RestartNeeded -or $RestartNeeded
            }
        }

        return $RestartNeeded
    }
}
else {
    $Reboot = $false
}

Invoke-Command -Session $Session -ArgumentList $RemoteFolder -ScriptBlock {
    New-Item -ItemType Directory -Path "$Using:RemoteFolder" -Force
}
Copy-Item -ToSession $Session -Path "$Bin/installer.msix"  -Destination $RemoteFolder -Force
Copy-Item -ToSession $Session -Path "$Bin/wsltests.dll"  -Destination $RemoteFolder -Force
Copy-Item -ToSession $Session -Path "$Bin/testplugin.dll"  -Destination $RemoteFolder -Force
Copy-Item -ToSession $Session -Path "$PSScriptRoot/test-setup.ps1"  -Destination $RemoteFolder -Force
Copy-Item -ToSession $Session -Path "$PSScriptRoot/run-tests.ps1"  -Destination $RemoteFolder -Force
Copy-Item -ToSession $Session -Path "$PSScriptRoot/../../test/linux/unit_tests" -Destination $RemoteFolder -Recurse -Force

if (!$SkipDistro) {
    Copy-Item -ToSession $Session -Path $TestDistroPath -Destination "$RemoteFolder/test_distro.tar.gz" -Force
}

$taefVersion = (Select-Xml -Path "$PSScriptRoot\..\..\packages.config" -XPath '/packages/package[@id=''Microsoft.Taef'']/@version').Node.Value
$taefPackage = "$ArtifactFolder/packages/Microsoft.Taef.$taefVersion/build/Binaries/$Platform"
Copy-Item -ToSession $Session -Path "$taefPackage" -Destination $TaefFolder -Recurse -Force
Invoke-Command -Session $Session -ArgumentList $RemoteFolder -ScriptBlock {
    $path = [System.Environment]::GetEnvironmentVariable("PATH", [EnvironmentVariableTarget]::User) -split ";"
    if ($path -notcontains $using:TaefFolder) {
        $path += $using:TaefFolder
        [System.Environment]::SetEnvironmentVariable("PATH", $path -join ";", [EnvironmentVariableTarget]::User)
    }
}


if ($Reboot) {
    Invoke-Command -Session $Session -ScriptBlock { Start-Process shutdown -ArgumentList "-r","-t 0" }
}