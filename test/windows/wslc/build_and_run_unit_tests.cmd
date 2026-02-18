@echo off
setlocal enabledelayedexpansion

:: Get the directory where this script is located
set SCRIPT_DIR=%~dp0

:: Parse arguments
set RUN_TESTS=1
set EXTRA_ARGS=

:parse_args
if "%~1"=="" goto end_parse
if /i "%~1"=="-SkipTests" (
    set RUN_TESTS=0
    shift
    goto parse_args
)
set EXTRA_ARGS=!EXTRA_ARGS! %1
shift
goto parse_args

:end_parse

:: Navigate to repo root (3 levels up from test\windows\wslc)
cd /d "%SCRIPT_DIR%..\..\..\\"

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

:: Run tests if not skipped
if "%RUN_TESTS%"=="1" (
    echo.
    echo ============================================================================
    echo Running WSLC CLI Tests
    echo ============================================================================
    echo.

    :: Run the existing test script, passing through all arguments
    call test\windows\wslc\run_unit_tests.cmd %EXTRA_ARGS%
    exit /b !errorlevel!
)

exit /b 0