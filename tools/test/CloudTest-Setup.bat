@echo off

rem Required to make cloudtest use our version of taef
setx /M CloudTestWorkerCustomTaefExe "%1\taef\te.exe"

mkdir %2\WexLogFileOutput
mklink /D %1\WexLogFileOutput %2\WexLogFileOutput

rem These need to run before the package is installed
powershell -NoProfile -Command "Add-MpPreference -ExclusionPath 'C:\Program Files\WSL'"
powershell -NoProfile -Command "Add-MpPreference -ExclusionProcess @('wsl.exe', 'wslg.exe', 'wslconfig.exe', 'wslrelay.exe', 'wslhost.exe', 'msrdc.exe', 'wslservice.exe', 'msal.wsl.proxy.exe', 'te.processhost.exe')"