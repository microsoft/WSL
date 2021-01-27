@echo off
:: Note: This script terminates WSL. Save your work before running it.

:: Check for administrator access
net session >nul 2>&1 || goto :admin

:: Validate that required files are here
if not exist wsl_networking.wprp (echo wsl_networking.wprp not found && exit /b 1)
if not exist networking.sh (echo networking.sh not found && exit /b 1)

:: List all HNS objects
echo HNS objects: 
hnsdiag list all -df

:: The WSL HNS network is created once per boot. Resetting it to collect network creation logs
echo Deleting HNS network
powershell.exe -NoProfile "Get-HnsNetwork | Where-Object {$_.Name -eq 'WSL'} | Remove-HnsNetwork"

:: Stop WSL
net.exe stop LxssManager

:: Start packet capture
powershell.exe -NoProfile "New-NetEventSession HnsPacketCapture -CaptureMode SaveToFile -LocalFilePath %cd%\\packets.etl" || goto :fail
powershell.exe -NoProfile "Add-NetEventPacketCaptureProvider -Level 5 -SessionName HnsPacketCapture -CaptureType BothPhysicalAndSwitch " || goto :fail
powershell.exe -NoProfile "Start-NetEventSession HnsPacketCapture" || goto :fail

:: Collect WSL logs
wpr -start wsl_networking.wprp -filemode || goto :fail
wsl.exe tr -d "\r" ^| bash < ./networking.sh

wpr -stop wsl.etl || goto :fail
powershell -NoProfile "Stop-NetEventSession HnsPacketCapture;  Remove-NetEventSession HnsPacketCapture" || goto :fail

exit /b 0

:fail
echo Failed to collect WSL logs
powershell -NoProfile "Stop-NetEventSession HnsPacketCapture; Remove-NetEventSession HnsPacketCapture"
exit /b 1

:admin
echo This script needs to run with administrative access
exit /b 1