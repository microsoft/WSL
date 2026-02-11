@echo off
setlocal

:: Get the directory where this script is located
set SCRIPT_DIR=%~dp0

:: Navigate to repo root (3 levels up from test\windows\wslc)
cd /d "%SCRIPT_DIR%..\..\..\"

echo.
echo ============================================================================
echo Building WSLC CLI Tests
echo ============================================================================
echo.

:: Configure if needed
if not exist "wsl.sln" (
    echo Configuring project...
    cmake .
    if errorlevel 1 (
        echo ERROR: CMake configuration failed
        exit /b 1
    )
    echo.
)

:: Build wsltests
echo Building wsltests...
cmake --build . --target wsltests
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo ============================================================================
echo Running WSLC CLI Tests
echo ============================================================================
echo.

:: Run the existing test script
call test\windows\wslc\run_unit_tests.cmd %*

exit /b %errorlevel%