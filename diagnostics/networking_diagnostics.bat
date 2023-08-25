:: Check for administrator privileges.
net session >nul 2>&1 || goto :admin

:: Validate that required files are here.
if not exist wsl_networking.wprp (echo wsl_networking.wprp not found && exit /b 1)
if not exist networking.sh (echo networking.sh not found && exit /b 1)

set networking_folder=".\networking_logs"
set neworking_logs_zip=".\WslNetworkingLogs.zip"

mkdir %networking_folder%

cd %networking_folder%

IF "%1"=="--stop-wsl" (
   :: The WSL HNS network is created once per boot. Resetting it to collect network creation logs.
   echo Deleting HNS network
   powershell.exe -NoProfile "Get-HnsNetwork | Where-Object {$_.Name -eq 'WSL'} | Remove-HnsNetwork"

   :: Stop WSL.
   net.exe stop WslService || net.exe stop LxssManager
)

wsl.exe tr -d "\r" ^| bash < ../networking.sh > wsl_network_configuration_before.log
powershell Get-NetRoute > get_netroute.log

powershell invoke-expression 'cmd /c start powershell -Command { ..\collect-wsl-logs.ps1 }'
powershell invoke-expression 'cmd /c start powershell -Command { "wsl.exe -u root sudo tcpdump -n -i any > tcpdump.log" }'

wpr -start .\wsl_networking.wprp -filemode -instanceName wpr_networking
pktmon start -c --flags 0x1A
netsh wfp capture start

pause

:: allow some time for the user to stop logs in all the spawned shells
timeout 20

netsh wfp capture stop
pktmon stop
wpr -stop wsl_networking.etl -instanceName wpr_networking

wsl.exe tr -d "\r" ^| bash < ../networking.sh > wsl_network_configuration_after.log

cd ..

del %neworking_logs_zip%
powershell Compress-Archive -Path %networking_folder% -DestinationPath %neworking_logs_zip%

rmdir /s /q %networking_folder%

echo "Finished log collection - please collect the zip archive from the path below"
powershell Resolve-Path %neworking_logs_zip%

exit /b 0

:: Error message if the user does not have administrative privileges.
:admin
echo This script needs to run with administrative privileges.
exit /b 1