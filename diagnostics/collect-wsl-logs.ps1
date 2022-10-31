#Requires -RunAsAdministrator

[CmdletBinding()]
Param (
    $LogProfile = $null,
    [switch]$Dump = $false
   )

Set-StrictMode -Version Latest

$folder = "WslLogs-" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
mkdir -p $folder

if ($LogProfile -eq $null)
{
    $LogProfile = "$folder/wsl.wprp"
    try {
        Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/wsl.wprp" -OutFile $LogProfile
    }
    catch {
        throw
    }
}

reg.exe export HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Lxss $folder/HKCU.reg
reg.exe export HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Lxss $folder/HKLM.reg
reg.exe export HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\P9NP $folder/P9NP.reg

$wslconfig = "$env:USERPROFILE/.wslconfig"
if (Test-Path $wslconfig)
{
    Copy-Item $wslconfig $folder
}

get-appxpackage MicrosoftCorporationII.WindowsSubsystemforLinux > $folder/appxpackage.txt
get-acl "C:\ProgramData\Microsoft\Windows\WindowsApps" | Format-List > $folder/acl.txt

wpr.exe -start $LogProfile -filemode 

Write-Host -NoNewLine -ForegroundColor Green "Log collection is running. Please reproduce the problem and press any key to save the logs."
$null = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown');

Write-Host "`nSaving logs..."

wpr.exe -stop $folder/logs.etl

if ($Dump)
{
    $Assembly = [PSObject].Assembly.GetType('System.Management.Automation.WindowsErrorReporting')
    $DumpMethod = $Assembly.GetNestedType('NativeMethods', 'NonPublic').GetMethod('MiniDumpWriteDump', [Reflection.BindingFlags] 'NonPublic, Static')

    $dumpFolder = Join-Path (Resolve-Path "$folder") dumps
    New-Item -ItemType "directory" -Path "$dumpFolder"

    $executables = "wsl", "wslservice", "wslhost", "msrdc"
    foreach($process in Get-Process | Where-Object { $executables -contains $_.ProcessName})
    {
        $dumpFile =  "$dumpFolder/$($process.ProcessName).$($process.Id).dmp"
        Write-Host "Writing $($dumpFile)"

        $OutputFile = New-Object IO.FileStream($dumpFile, [IO.FileMode]::Create)

        $Result = $DumpMethod.Invoke($null, @($process.Handle,
                                              $process.id,
                                              $OutputFile.SafeFileHandle,
                                              [UInt32] 2
                                              [IntPtr]::Zero,
                                              [IntPtr]::Zero,
                                              [IntPtr]::Zero))

        $OutputFile.Close()
        if (-not $Result)
        {
            Write-Host "Failed to write dump for: $($dumpFile)"
        }
    }
}

$logArchive = "$(Resolve-Path $folder).zip"
Compress-Archive -Path $folder -DestinationPath $logArchive
Remove-Item $folder -Recurse

Write-Host -ForegroundColor Green "Logs saved in: $logArchive. Please attach that file to the GitHub issue."
