set networking_folder=".\networking_logs"
set neworking_logs_zip=".\WslNetworkingLogs.zip"

mkdir %networking_folder%

wsl.exe tr -d "\r" ^| bash < ./networking.sh > %networking_folder%\wsl_network_configuration_before.log
powershell Get-NetRoute > %networking_folder%\get_netroute.log

powershell invoke-expression 'cmd /c start powershell -Command { .\collect-wsl-logs.ps1 }'
powershell invoke-expression 'cmd /c start powershell -Command { "wsl.exe sudo tcpdump -n -i any > tcpdump.log" }'

wpr -start .\wsl_networking.wprp -filemode -instanceName wpr_networking
pktmon start -c --flags 0x1A
netsh wfp capture start

pause

:: allow some time for the user to stop logs in all the spawned shells
:: (particularly the shell running collect-wsl-logs.ps1 will take a bit to stop)
timeout 20

netsh wfp capture stop
pktmon stop
wpr -stop %networking_folder%\wsl_networking.etl -instanceName wpr_networking

wsl.exe tr -d "\r" ^| bash < ./networking.sh > %networking_folder%\wsl_network_configuration_after.log

move tcpdump.log %networking_folder%
move PktMon.etl %networking_folder%
move wfpdiag.cab %networking_folder%
move "*zip" %networking_folder%

powershell Compress-Archive -Path %networking_folder% -DestinationPath %neworking_logs_zip%

rmdir /s /q %networking_folder%

echo "Finished log collection"