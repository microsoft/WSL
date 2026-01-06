#Requires -RunAsAdministrator

[CmdletBinding()]
Param (
    $LogProfile = $null,
    [switch]$Dump = $false
   )

Set-StrictMode -Version Latest

$folder = "WslLogs-" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
mkdir -p $folder | Out-Null

# Check if LogProfile is a custom file path or a profile name
if ($LogProfile -ne $null -And [System.IO.File]::Exists($LogProfile))
{
    # User provided a custom .wprp file path - use it directly
    $wprpFile = $LogProfile
    $wprpProfile = $null  # Use default profile in the file
}
else
{
    # Map log profile names to WPRP profile names
    $wprpProfile = "WSL"
    if ($LogProfile -eq "storage")
    {
        $wprpProfile = "WSL-Storage"
    }
    elseif ($LogProfile -eq "networking")
    {
        $wprpProfile = "WSL-Networking"
    }
    elseif ($LogProfile -eq "hvsocket")
    {
        $wprpProfile = "WSL-HvSocket"
    }
    elseif ($LogProfile -ne $null)
    {
        Write-Error "Unknown log profile: $LogProfile. Valid options are: storage, networking, hvsocket, or a path to a custom .wprp file"
        exit 1
    }

    # Use the consolidated wsl.wprp file, attempt to use local copy first.
    $wprpFile = "$folder/wsl.wprp"
    if (Test-Path "$PSScriptRoot/wsl.wprp")
    {
        Write-Host -ForegroundColor Yellow "Using local copy of wsl.wprp"
        Copy-Item "$PSScriptRoot/wsl.wprp" $wprpFile
    }
    else
    {
        Write-Host -ForegroundColor Yellow "wsl.wprp not found in the current directory. Downloading it from GitHub."
        Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/wsl.wprp" -OutFile $wprpFile
    }
}

reg.exe export HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Lxss $folder/HKCU.txt 2>&1 | Out-Null
reg.exe export HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Lxss $folder/HKLM.txt 2>&1 | Out-Null
reg.exe export HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\P9NP $folder/P9NP.txt 2>&1 | Out-Null
reg.exe export HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\WinSock2 $folder/Winsock2.txt 2>&1 | Out-Null
reg.exe export "HKEY_CLASSES_ROOT\CLSID\{e66b0f30-e7b4-4f8c-acfd-d100c46c6278}" $folder/wslsupport-proxy.txt 2>&1 | Out-Null
reg.exe export "HKEY_CLASSES_ROOT\CLSID\{a9b7a1b9-0671-405c-95f1-e0612cb4ce7e}" $folder/wslsupport-impl.txt 2>&1 | Out-Null
Get-ItemProperty -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" > $folder/windows-version.txt

Get-Service wslservice -ErrorAction Ignore | Format-list * -Force  > $folder/wslservice.txt

$wslconfig = "$env:USERPROFILE/.wslconfig"
if (Test-Path $wslconfig)
{
    Copy-Item $wslconfig $folder | Out-Null
}

Copy-Item "C:\Windows\temp\wsl-install-log.txt" $folder -ErrorAction ignore

get-appxpackage MicrosoftCorporationII.WindowsSubsystemforLinux -ErrorAction Ignore > $folder/appxpackage.txt
get-acl "C:\ProgramData\Microsoft\Windows\WindowsApps" -ErrorAction Ignore | Format-List > $folder/acl.txt
Get-WindowsOptionalFeature -Online > $folder/optional-components.txt
bcdedit.exe > $folder/bcdedit.txt

$uninstallLogs = "$env:TEMP/wsl-uninstall-logs.txt"
if (Test-Path $uninstallLogs)
{
    Copy-Item $uninstallLogs $folder | Out-Null
}

$wprOutputLog = "$folder/wpr.txt"

# Build wpr command - if wprpProfile is set, use profile syntax, otherwise use file only
if ($wprpProfile -ne $null)
{
    $wprCommand = "$wprpFile!$wprpProfile"
}
else
{
    $wprCommand = $wprpFile
}

wpr.exe -start $wprCommand -filemode 2>&1 >> $wprOutputLog
if ($LastExitCode -Ne 0)
{
    Write-Host -ForegroundColor Yellow "Log collection failed to start (exit code: $LastExitCode), trying to reset it."
    wpr.exe -cancel 2>&1 >> $wprOutputLog

    wpr.exe -start $wprCommand -filemode 2>&1 >> $wprOutputLog
    if ($LastExitCode -Ne 0)
    {
        Write-Host -ForegroundColor Red "Couldn't start log collection (exitCode: $LastExitCode)"
    }
}

try
{
    Write-Host -NoNewLine "Log collection is running. Please "
    Write-Host -NoNewLine -ForegroundColor Red "reproduce the problem "
    Write-Host -NoNewLine "and once done press any key to save the logs."

    $KeysToIgnore =
          16,  # Shift (left or right)
          17,  # Ctrl (left or right)
          18,  # Alt (left or right)
          20,  # Caps lock
          91,  # Windows key (left)
          92,  # Windows key (right)
          93,  # Menu key
          144, # Num lock
          145, # Scroll lock
          166, # Back
          167, # Forward
          168, # Refresh
          169, # Stop
          170, # Search
          171, # Favorites
          172, # Start/Home
          173, # Mute
          174, # Volume Down
          175, # Volume Up
          176, # Next Track
          177, # Previous Track
          178, # Stop Media
          179, # Play
          180, # Mail
          181, # Select Media
          182, # Application 1
          183  # Application 2

    $Key = $null
    while ($Key -Eq $null -Or $Key.VirtualKeyCode -Eq $null -Or $KeysToIgnore -Contains $Key.VirtualKeyCode)
    {
        $Key = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
    }

    Write-Host "`nSaving logs..."
}
finally
{
    wpr.exe -stop $folder/logs.etl 2>&1 >> $wprOutputLog
}

if ($Dump)
{
    $Assembly = [PSObject].Assembly.GetType('System.Management.Automation.WindowsErrorReporting')
    $DumpMethod = $Assembly.GetNestedType('NativeMethods', 'NonPublic').GetMethod('MiniDumpWriteDump', [Reflection.BindingFlags] 'NonPublic, Static')

    $dumpFolder = Join-Path (Resolve-Path "$folder") dumps
    New-Item -ItemType "directory" -Path "$dumpFolder"

    $executables = "wsl", "wslservice", "wslhost", "msrdc", "dllhost"
    foreach($process in Get-Process | Where-Object { $executables -contains $_.ProcessName})
    {
        $dumpFile =  "$dumpFolder/$($process.ProcessName).$($process.Id).dmp"
        Write-Host "Writing $($dumpFile)"

        $OutputFile = New-Object IO.FileStream($dumpFile, [IO.FileMode]::Create)

        $Result = $DumpMethod.Invoke($null, @($process.Handle,
                                              $process.id,
                                              $OutputFile.SafeFileHandle,
                                              [UInt32] 2,
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
