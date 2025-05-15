@echo off

rem Required to make cloudtest use our version of taef
setx /M CloudTestWorkerCustomTaefExe "%1\taef\te.exe"

mkdir %2\WexLogFileOutput
mklink /D %1\WexLogFileOutput %2\WexLogFileOutput