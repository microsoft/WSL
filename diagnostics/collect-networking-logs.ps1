#Requires -RunAsAdministrator

[CmdletBinding()]
Param (
    [switch]$RestartWslReproMode = $false
   )

$folder = "WslNetworkingLogs-" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
mkdir -p $folder

# Retrieve WSL version and wslconfig file
get-appxpackage MicrosoftCorporationII.WindowsSubsystemforLinux > $folder/appxpackage.txt

$wslconfig = "$env:USERPROFILE/.wslconfig"
if (Test-Path $wslconfig)
{
    Copy-Item $wslconfig $folder
}

# Collect Linux network state before the repro
& wsl.exe -e ./networking.sh 2>&1 > $folder/linux_network_configuration_before.log

if ($RestartWslReproMode)
{
    # The WSL HNS network is created once per boot. Resetting it to collect network creation logs.
    Get-HnsNetwork | Where-Object {$_.Name -eq 'WSL'} | Remove-HnsNetwork

    # Stop WSL.
    net.exe stop WslService
    if(-not $?)
    {
        net.exe stop LxssManager
    }
}

# Start logging.
$logProfile = ".\wsl_networking.wprp"
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
    $tcpdumpProcess = Start-Process wsl.exe -ArgumentList "-u root tcpdump -n -i any > $folder/tcpdump.log" -PassThru
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
    while ($Key -Eq $null -Or $Key.VirtualKeyCode -Eq $null -Or $KeysToIgnore -Contains $Key.VirtualKeyCode)
    {
        $Key = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
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

# Collect Linux network state after the repro
& wsl.exe -e ./networking.sh 2>&1 > $folder/linux_network_configuration_after.log

# Collect host networking state relevant for WSL
# Using a try/catch for commands below, as some of them do not exist on all OS versions

try
{
    Get-NetAdapter -includeHidden | select Name,ifIndex,NetLuid,InterfaceGuid,Status,MacAddress,MtuSize,InterfaceType,Hidden,HardwareInterface,ConnectorPresent,MediaType,PhysicalMediaType | Out-File -FilePath "$folder/Get-NetAdapter.log" -Append
}
catch {}

try
{
    Get-NetIPConfiguration -All -Detailed | Out-File -FilePath "$folder/Get-NetIPConfiguration.log" -Append
}
catch {}

try
{
    Get-NetRoute | Out-File -FilePath "$folder/Get-NetRoute.log" -Append
}
catch {}

try
{
    Get-NetFirewallHyperVVMCreator | Out-File -FilePath "$folder/Get-NetFirewallHyperVVMCreator.log" -Append
}
catch {}

try
{
    Get-NetFirewallHyperVVMSetting -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVVMSetting_ActiveStore.log" -Append
}
catch {}

try
{
    Get-NetFirewallHyperVProfile -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVProfile_ActiveStore.log" -Append
}
catch {}

try
{
    Get-NetFirewallHyperVRule -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallHyperVRule_ActiveStore.log" -Append
}
catch {}

try
{
    Get-NetFirewallRule -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallRule_ActiveStore.log" -Append
}
catch {}

try
{
    Get-NetFirewallProfile -PolicyStore ActiveStore | Out-File -FilePath "$folder/Get-NetFirewallProfile_ActiveStore.log" -Append
}
catch {}

try
{
    Get-NetFirewallHyperVPort | Out-File -FilePath "$folder/Get-NetFirewallHyperVPort.log" -Append
}
catch {}

try
{
    & hnsdiag.exe list all 2>&1 > $folder/hnsdiag_list_all.log
}
catch {}

try
{
    & hnsdiag.exe list endpoints -df 2>&1 > $folder/hnsdiag_list_endpoints.log
}
catch {}

try
{
    foreach ($port in Get-NetFirewallHyperVPort)
    {
		& vfpctrl.exe /port $port.PortName /get-port-state 2>&1 > "$folder/vfp-port-$($port.PortName)-get-port-state.log"
	    & vfpctrl.exe /port $port.PortName /list-rule 2>&1 > "$folder/vfp-port-$($port.PortName)-list-rule.log"
    }
}
catch {}

try
{
    & vfpctrl.exe /list-vmswitch-port 2>&1 > $folder/vfpctrl_list_vmswitch_port.log
}
catch {}

$logArchive = "$(Resolve-Path $folder).zip"
Compress-Archive -Path $folder -DestinationPath $logArchive
Remove-Item $folder -Recurse

Write-Host -ForegroundColor Green "Logs saved in: $logArchive. Please attach that file to the GitHub issue."