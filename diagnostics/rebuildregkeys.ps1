# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License. See LICENSE in the project root for license information.

<#
Script to rebuild the WSL2 distros regkeys when they are missing from user's registry but provided in Get-AppxPackage
distros information are stored in HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss
Get-AppxPackage returns all installed packages
If WSL 2 distros are missing from registry but provided in Get-AppxPackage, this script can rebuild the registy and saved as wsl.reg
then user can import it to recover the regkeys

Example:
D:\repo\WSL\diagnostics>powershell .\rebuildregkeys.ps1
Below distros are detected in registry
Below apps are installed
  Ubuntu
Below apps are installed but not detected in registry which need to rebuild
  Ubuntu

File D:\repo\WSL\diagnostics\wsl.reg is generated, please import it to Windows registry

DefaultUid is set 0(root) in the script. After you imported the registry, you may reconfig the --default-user with the command the application provided.
This an example for ubuntu:
    ubuntu --help
    ubuntu config --default-user demo

#>

$defaultUidMesasge = @"
DefaultUid is set 0(root) in the script. After you imported the registry, you may reconfig the --default-user with the command the application provided.
This an example for ubuntu:
    ubuntu --help
    ubuntu config --default-user demo
"@

$template = @"
[HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Lxss]
[HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Lxss\{UUID}]
"State"=dword:00000001
"DistributionName"="{DISTRONAME}"
"Version"=dword:00000002
"BasePath"="{BASEPATH}"
"Flags"=dword:0000000f
"DefaultUid"=dword:00000000
"PackageFamilyName"="{PACKAGEFAMILY}"
"@

$activeDistros = Get-ChildItem -Path 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss' | Where-Object {$_.Name -match '{*-*-*-*-*}'} |Select-Object -Property Name

$activeDistroNames = New-Object -TypeName 'System.Collections.ArrayList';
$activeDistrosPackageFamilyNames = New-Object -TypeName 'System.Collections.ArrayList';
foreach ($distro in $activeDistros)
{
    $name = Get-ItemPropertyValue -Path "HKCU:$($distro.Name)" -Name DistributionName
    $activeDistroNames.Add($name) |Out-Null

    $packageName = Get-ItemPropertyValue -Path "HKCU:$($distro.Name)" -Name PackageFamilyName
    $activeDistrosPackageFamilyNames.Add($packageName) |Out-Null
}

Write-Host "Below distros are detected in registry"
foreach ($name in $activeDistroNames)
{
    Write-Host "  $name"
}

$hash = @{}
$hash.Add("CanonicalGroupLimited.UbuntuonWindows_79rhkp1fndgsc", "Ubuntu")
$hash.Add("TheDebianProject.DebianGNULinux_76v4gfsz19hv4","Debian")
$hash.Add("KaliLinux.54290C8133FEE_ey8k8hqnwqnmg", "kali-linux")
$hash.Add("46932SUSE.openSUSELeap42.2_022rs5jcyhyac", "openSUSE-42")
$hash.Add("46932SUSE.SUSELinuxEnterpriseServer12SP2_022rs5jcyhyac", "SLES-12")
$hash.Add("CanonicalGroupLimited.Ubuntu16.04onWindows_79rhkp1fndgsc","Ubuntu-16.04")
$hash.Add("CanonicalGroupLimited.Ubuntu18.04onWindows_79rhkp1fndgsc", "Ubuntu-18.04")
$hash.Add("CanonicalGroupLimited.Ubuntu20.04onWindows_79rhkp1fndgsc", "Ubuntu-20.04")

$allInstalledPackages = Get-AppxPackage

$basePaths = @{}
foreach ($knownPackage in $hash.Keys)
{
    $installed = $allInstalledPackages |Where-Object { $_.PackageFamilyName -eq $knownPackage }
    if ($installed)
    {
        $basePaths.Add($installed.PackageFamilyName, $installed.InstallLocation)
    }
}

Write-Host "Below apps are installed"
foreach ($key in $basePaths.keys)
{
    $value = $hash[$key]
    Write-Host "  $value"
}

Write-Host "Below apps are installed but not detected in registry which need to rebuild"

$rebuildPackages = New-Object -TypeName 'System.Collections.ArrayList'
foreach ($key in $basePaths.keys)
{
    $isInReg = $activeDistrosPackageFamilyNames | Where-Object {$_ -eq $key}
    if (!($isInReg))
    {
        $value = $hash[$key]
        $rebuildPackages.Add($key) | Out-Null
        Write-Host "  $value"
    }
}

$regConent = "Windows Registry Editor Version 5.00`r`n"
function Rebuild-RegKeys()
{
   param (
        [string]$DistroName,
        [string]$BasePath,
        [string]$PackageFamilyName
    )

    $uuid = [guid]::NewGuid()
    $content = $template.Replace("UUID", $uuid)
    $content = $content.Replace("{DISTRONAME}", "$DistroName");
    $content = $content.Replace("{PACKAGEFAMILY}", "$PackageFamilyName")
    $BasePath = $BasePath.Replace("\", "\\")
    $content = $content.Replace("{BASEPATH}", "$BasePath")
    "`r`n" + $content 
}

if ($rebuildPackages.Count -gt 0)
{
    $wslreg = "$($PSScriptRoot)\wsl.reg"
    

    $profile = $env:USERPROFILE
    foreach ($package in $rebuildPackages)
    {
        $basePath = "$profile\AppData\Local\Packages\$package\LocalState"
        $distroName = $hash[$package]
        $packageFamilyName = $package
        if (Test-Path $basePath)
        {
            $regConent += Rebuild-RegKeys "$distroName" "$basePath" "$packageFamilyName"
        }
    }
    Set-Content $wslreg $regConent -Encoding UTF8

    Write-Host "`r`nFile $wslreg is generated, please import it to Windows registry`r`n"
    Write-Host $defaultUidMesasge
}
else
{
    Write-Host "You don't need to rebuild any package"
}
