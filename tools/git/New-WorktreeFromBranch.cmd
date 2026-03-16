@echo off
REM New-WorktreeFromBranch.cmd - Convenience wrapper for human users
REM Master source: .github/skills/worktree-manager/scripts/New-WorktreeFromBranch.ps1
setlocal
for /f "delims=" %%i in ('git rev-parse --show-toplevel 2^>nul') do set REPO_ROOT=%%i
if "%REPO_ROOT%"=="" (
    echo Error: Not inside a Git repository.
    exit /b 1
)
where pwsh >nul 2>nul
if %ERRORLEVEL%==0 (
    set "PS_EXE=pwsh"
) else (
    set "PS_EXE=powershell.exe"
)
"%PS_EXE%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\.github\skills\worktree-manager\scripts\New-WorktreeFromBranch.ps1" %*
