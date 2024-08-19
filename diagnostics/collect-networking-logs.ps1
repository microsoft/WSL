#Requires -RunAsAdministrator

[CmdletBinding()]
Param (
    [switch]$RestartWslReproMode = $false
   )

function Collect-WindowsNetworkState {

    param (
        $ReproStep
    )

    # Collect host networking state relevant for WSL
    # Using a try/catch for commands below, as some of them do not exist on all OS versions

    try
    {
        Get-NetAdapter -includeHidden | select Name,ifIndex,NetLuid,InterfaceGuid,Status,MacAddress,MtuSize,InterfaceType,Hidden,HardwareInterface,ConnectorPresent,MediaType,PhysicalMediaType | Out-File -FilePath "$folder/Get-NetAdapter_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        & netsh nlm query all $folder/nlmquery_"$ReproStep".log
    }
    catch {}

    try
    {
        Get-NetIPConfiguration -All -Detailed | Out-File -FilePath "$folder/Get-NetIPConfiguration_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetRoute | Out-File -FilePath "$folder/Get-NetRoute_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallHyperVVMCreator | Out-File -FilePath "$folder/Get-NetFirewallHyperVVMCreator_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallHyperVVMSetting -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVVMSetting_ActiveStore_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallHyperVProfile -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVProfile_ActiveStore_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallHyperVRule -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVRule_ActiveStore_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallRule -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallRule_ActiveStore_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallProfile -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallProfile_ActiveStore_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetFirewallHyperVPort | Out-File -FilePath "$folder/Get-NetFirewallHyperVPort_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        & hnsdiag.exe list all 2>&1 > $folder/hnsdiag_list_all_"$ReproStep".log
    }
    catch {}

    try
    {
        & hnsdiag.exe list endpoints -df 2>&1 > $folder/hnsdiag_list_endpoints_"$ReproStep".log
    }
    catch {}

    try
    {
        foreach ($port in Get-NetFirewallHyperVPort)
        {
            & vfpctrl.exe /port $port.PortName /get-port-state 2>&1 > "$folder/vfp-port-$($port.PortName)-get-port-state_$ReproStep.log"
            & vfpctrl.exe /port $port.PortName /list-rule 2>&1 > "$folder/vfp-port-$($port.PortName)-list-rule_$ReproStep.log"
        }
    }
    catch {}

    try
    {
        & vfpctrl.exe /list-vmswitch-port 2>&1 > $folder/vfpctrl_list_vmswitch_port_"$ReproStep".log
    }
    catch {}

    try
    {
        Get-VMSwitch | select Name,Id,SwitchType | Out-File -FilePath "$folder/Get-VMSwitch_$ReproStep.log" -Append
    }
    catch {}

    try
    {
        Get-NetUdpEndpoint | Out-File -FilePath "$folder/Get-NetUdpEndpoint_$ReproStep.log" -Append
    }
    catch {}
}

$folder = "WslNetworkingLogs-" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
mkdir -p $folder

$logProfile = "$folder/wsl_networking.wprp"
$networkingBashScript = "$folder/networking.sh"

# Copy/Download supporting files
if (Test-Path "$PSScriptRoot/wsl_networking.wprp")
{
    Copy-Item "$PSScriptRoot/wsl_networking.wprp" $logProfile
}
else
{
    Write-Host -ForegroundColor Yellow "wsl_networking.wprp not found in the current directory. Downloading it from GitHub."
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/wsl_networking.wprp" -OutFile $logProfile
}

if (Test-Path "$PSScriptRoot/networking.sh")
{
    Copy-Item "$PSScriptRoot/networking.sh" $networkingBashScript
}
else
{
    Write-Host -ForegroundColor Yellow "networking.sh not found in the current directory. Downloading it from GitHub."
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/networking.sh" -OutFile $networkingBashScript
}

# Retrieve WSL version and wslconfig file
get-appxpackage MicrosoftCorporationII.WindowsSubsystemforLinux > $folder/appxpackage.txt

$wslconfig = "$env:USERPROFILE/.wslconfig"
if (Test-Path $wslconfig)
{
    Copy-Item $wslconfig $folder
}

# Collect Linux & Windows network state before the repro
& wsl.exe -u root -e $networkingBashScript 2>&1 > $folder/linux_network_configuration_before.log

Collect-WindowsNetworkState "before_repro"

if ($RestartWslReproMode)
{
    # The WSL HNS network is created once per boot. Resetting it to collect network creation logs.
    # Note: The below HNS command applies only to WSL in NAT mode
    Get-HnsNetwork | Where-Object {$_.Name -eq 'WSL' -Or $_.Name -eq 'WSL (Hyper-V firewall)'} | Remove-HnsNetwork

    # Stop WSL.
    net.exe stop WslService
    if(-not $?)
    {
        net.exe stop LxssManager
    }
}

# Start logging.
$wprOutputLog = "$folder/wpr.txt"

wpr.exe -start $logProfile -filemode 2>&1 >> $wprOutputLog
if ($LastExitCode -Ne 0)
{
    Write-Host -ForegroundColor Yellow "Log collection failed to start (exit code: $LastExitCode), trying to reset it."
    wpr.exe -cancel 2>&1 >> $wprOutputLog

    wpr.exe -start $logProfile -filemode 2>&1 >> $wprOutputLog
    if ($LastExitCode -Ne 0)
    {
        Write-Host -ForegroundColor Red "Couldn't start log collection (exitCode: $LastExitCode)"
    }
}

# Start packet capture using pktmon
pktmon start -c --flags 0x1A --file-name "$folder/pktmon.etl" | out-null

# Start WFP capture
netsh wfp capture start file="$folder/wfpdiag.cab"

# Start tcpdump. Using a try/catch as tcpdump might not be installed
$tcpdumpProcess = $null
try
{
    $tcpdumpProcess = Start-Process wsl.exe -ArgumentList "-u root tcpdump -n -i any -e -vvv > $folder/tcpdump.log" -PassThru
}
catch {}

try
{
    Write-Host -NoNewLine -ForegroundColor Green "Log collection is running. Please reproduce the problem and press any key to save the logs."

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
    while (($Key -Eq $null -Or $Key.VirtualKeyCode -Eq $null -Or $KeysToIgnore -Contains $Key.VirtualKeyCode) -and ($null -eq $tcpdumpProcess -or $tcpdumpProcess.HasExited -eq $false))
    {
        if ([console]::KeyAvailable)
        {
            $Key = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
        }
        else
        {
            Start-Sleep -Seconds 1
        }
    }

    Write-Host "`nSaving logs..."
}
finally
{
    try
    {
        wsl.exe -u root killall tcpdump
        if ($tcpdumpProcess -ne $null)
        {
            Wait-Process -InputObject $tcpdumpProcess -Timeout 10
        }
    }
    catch {}

    netsh wfp capture stop
    pktmon stop | out-null
    wpr.exe -stop $folder/logs.etl 2>&1 >> $wprOutputLog
}

# Collect Linux & Windows network state after the repro
& wsl.exe -u root -e $networkingBashScript 2>&1 > $folder/linux_network_configuration_after.log

Collect-WindowsNetworkState "after_repro"

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

Remove-Item $logProfile
Remove-Item $networkingBashScript

$logArchive = "$(Resolve-Path $folder).zip"
Compress-Archive -Path $folder -DestinationPath $logArchive
Remove-Item $folder -Recurse

Write-Host -ForegroundColor Green "Logs saved in: $logArchive. Please attach that file to the GitHub issue."
