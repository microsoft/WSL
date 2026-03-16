@echo off
REM New-WorktreeFromIssue.cmd - Convenience wrapper for human users
REM Master source: .github/skills/worktree-manager/scripts/New-WorktreeFromIssue.ps1
setlocal
for /f "delims=" %%i in ('git rev-parse --show-toplevel 2^>nul') do set REPO_ROOT=%%i
if "%REPO_ROOT%"=="" (
    echo Error: Not inside a Git repository.
    exit /b 1
)
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\.github\skills\worktree-manager\scripts\New-WorktreeFromIssue.ps1" %*
