powershell invoke-expression 'cmd /c start powershell -Command { .\collect-wsl-logs.ps1 }'
wsl.exe tr -d "\r" ^| bash < ./networking.sh > wsl_network_configuration_before.log
powershell Get-NetRoute > get_netroute.log
powershell invoke-expression 'cmd /c start powershell -Command { "wsl.exe sudo tcpdump -n -i any > tcpdump.log" }'

wpr -start .\wsl_networking.wprp -filemode -instanceName wpr_networking
pktmon start -c --flags 0x1A
netsh wfp capture start

pause

netsh wfp capture stop
pktmon stop
wpr -stop wsl_networking.etl -instanceName wpr_networking

wsl.exe tr -d "\r" ^| bash < ./networking.sh > wsl_network_configuration_after.log

echo "Finished log collection"