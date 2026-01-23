#Requires -RunAsAdministrator

[CmdletBinding()]
Param (
    $LogProfile = $null,
    [switch]$Dump = $false,
    [switch]$RestartWslReproMode = $false
   )

Set-StrictMode -Version Latest

function Collect-WindowsNetworkState {
    param (
        $Folder,
        $ReproStep
    )

    # Collect host networking state relevant for WSL
    # Using a try/catch for commands below, as some of them do not exist on all OS versions

    try { Get-NetAdapter -includeHidden | select Name,ifIndex,NetLuid,InterfaceGuid,Status,MacAddress,MtuSize,InterfaceType,Hidden,HardwareInterface,ConnectorPresent,MediaType,PhysicalMediaType | Out-File -FilePath "$Folder/Get-NetAdapter_$ReproStep.log" -Append } catch {}
    try { & netsh nlm query all $Folder/nlmquery_"$ReproStep".log } catch {}
    try { Get-NetIPConfiguration -All -Detailed | Out-File -FilePath "$Folder/Get-NetIPConfiguration_$ReproStep.log" -Append } catch {}
    try { Get-NetRoute | Out-File -FilePath "$Folder/Get-NetRoute_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallHyperVVMCreator | Out-File -FilePath "$Folder/Get-NetFirewallHyperVVMCreator_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallHyperVVMSetting -PolicyStore ActiveStore | Out-File -FilePath "$Folder/Get-NetFirewallHyperVVMSetting_ActiveStore_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallHyperVProfile -PolicyStore ActiveStore | Out-File -FilePath "$Folder/Get-NetFirewallHyperVProfile_ActiveStore_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallHyperVRule -PolicyStore ActiveStore | Out-File -FilePath "$Folder/Get-NetFirewallHyperVRule_ActiveStore_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallRule -PolicyStore ActiveStore | Out-File -FilePath "$Folder/Get-NetFirewallRule_ActiveStore_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallProfile -PolicyStore ActiveStore | Out-File -FilePath "$Folder/Get-NetFirewallProfile_ActiveStore_$ReproStep.log" -Append } catch {}
    try { Get-NetFirewallHyperVPort | Out-File -FilePath "$Folder/Get-NetFirewallHyperVPort_$ReproStep.log" -Append } catch {}
    try { & hnsdiag.exe list all 2>&1 > $Folder/hnsdiag_list_all_"$ReproStep".log } catch {}
    try { & hnsdiag.exe list endpoints -df 2>&1 > $Folder/hnsdiag_list_endpoints_"$ReproStep".log } catch {}
    try {
        foreach ($port in Get-NetFirewallHyperVPort) {
            & vfpctrl.exe /port $port.PortName /get-port-state 2>&1 > "$Folder/vfp-port-$($port.PortName)-get-port-state_$ReproStep.log"
            & vfpctrl.exe /port $port.PortName /list-rule 2>&1 > "$Folder/vfp-port-$($port.PortName)-list-rule_$ReproStep.log"
        }
    } catch {}
    try { & vfpctrl.exe /list-vmswitch-port 2>&1 > $Folder/vfpctrl_list_vmswitch_port_"$ReproStep".log } catch {}
    try { Get-VMSwitch | select Name,Id,SwitchType | Out-File -FilePath "$Folder/Get-VMSwitch_$ReproStep.log" -Append } catch {}
    try { Get-NetUdpEndpoint | Out-File -FilePath "$Folder/Get-NetUdpEndpoint_$ReproStep.log" -Append } catch {}
}

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
        Copy-Item "$PSScriptRoot/wsl.wprp" $wprpFile
    }
    else
    {
        Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/wsl.wprp" -OutFile $wprpFile
    }
}

# Networking-specific setup
if ($LogProfile -eq "networking")
{
    # Copy/download networking.sh script
    $networkingBashScript = "$folder/networking.sh"
    if (Test-Path "$PSScriptRoot/networking.sh")
    {
        Copy-Item "$PSScriptRoot/networking.sh" $networkingBashScript
    }
    else
    {
        Write-Host -ForegroundColor Yellow "networking.sh not found in the current directory. Downloading it from GitHub."
        Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/networking.sh" -OutFile $networkingBashScript
    }

    # Detect the super user (uid=0, not necessarily named "root" - see #11693)
    $superUser = & wsl.exe -- id -nu 0

    # Collect Linux & Windows network state before the repro
    & wsl.exe -u $superUser -e $networkingBashScript 2>&1 > $folder/linux_network_configuration_before.log
    Collect-WindowsNetworkState -Folder $folder -ReproStep "before_repro"

    if ($RestartWslReproMode)
    {
        # The WSL HNS network is created once per boot. Resetting it to collect network creation logs.
        # Note: The below HNS command applies only to WSL in NAT mode
        Get-HnsNetwork | Where-Object {$_.Name -eq 'WSL' -Or $_.Name -eq 'WSL (Hyper-V firewall)'} | Remove-HnsNetwork

        # Stop WSL
        net.exe stop WslService
        if(-not $?) { net.exe stop LxssManager }
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

# Start networking-specific captures
$tcpdumpProcess = $null
if ($LogProfile -eq "networking")
{
    pktmon start -c --flags 0x1A --file-name "$folder/pktmon.etl" | out-null
    netsh wfp capture start file="$folder/wfpdiag.cab"

    # Ensure WSL is running before collecting network state
    & wsl.exe -- true 2>&1 | Out-Null

    # Start tcpdump (may not be installed)
    try
    {
        $tcpdumpProcess = Start-Process wsl.exe -ArgumentList "-u $superUser tcpdump -n -i any -e -vvv > $folder/tcpdump.log" -WindowStyle Hidden -PassThru
    }
    catch {}
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
    # Stop networking-specific captures
    if ($LogProfile -eq "networking")
    {
        try
        {
            wsl.exe -u $superUser killall tcpdump
            if ($tcpdumpProcess -ne $null)
            {
                Wait-Process -InputObject $tcpdumpProcess -Timeout 10
            }
        }
        catch {}

        netsh wfp capture stop
        pktmon stop | out-null
    }

    wpr.exe -stop $folder/logs.etl 2>&1 >> $wprOutputLog
}

# Networking-specific post-repro collection
if ($LogProfile -eq "networking")
{
    # Collect Linux & Windows network state after the repro
    & wsl.exe -u $superUser -e $networkingBashScript 2>&1 > $folder/linux_network_configuration_after.log
    Collect-WindowsNetworkState -Folder $folder -ReproStep "after_repro"

    try
    {
        # Collect HNS events from past 24 hours
        $events = Get-WinEvent -ProviderName Microsoft-Windows-Host-Network-Service | Where-Object { $_.TimeCreated -ge ((Get-Date) - (New-TimeSpan -Day 1)) }
        ($events | ForEach-Object { '{0},{1},{2},{3}' -f $_.TimeCreated, $_.Id, $_.LevelDisplayName, $_.Message }) -join [environment]::NewLine | Out-File -FilePath "$folder/hns_events.log" -Append
    }
    catch {}

    # Collect the old Tcpip6 registry values - as they can break WSL if DisabledComponents is set to 0xff
    # see https://learn.microsoft.com/en-us/troubleshoot/windows-server/networking/configure-ipv6-in-windows
    try
    {
        Get-Item HKLM:SYSTEM\CurrentControlSet\Services\Tcpip6\Parameters | Out-File -FilePath "$folder/tcpip6_parameters.log" -Append
    }
    catch {}

    # Collect the setup and NetSetup log files
    $netSetupPath = "$env:WINDIR/logs/netsetup"
    if (Test-Path $netSetupPath)
    {
        Copy-Item $netSetupPath/* $folder
    }

    $setupApiPath = "$env:WINDIR/inf/setupapi.dev.log"
    if (Test-Path $setupApiPath)
    {
        Copy-Item $setupApiPath $folder
    }

    Remove-Item $networkingBashScript
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
