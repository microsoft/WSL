@echo off
REM Fast unit test runner wrapper for WSLC CLI

set SCRIPT_DIR=%~dp0
set PS_SCRIPT=%SCRIPT_DIR%run_unit_tests.ps1

REM Pass all arguments to the PowerShell script
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*