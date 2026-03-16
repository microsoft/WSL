@echo off
REM Delete-Worktree.cmd - Convenience wrapper for human users
REM Master source: .github/skills/worktree-manager/scripts/Delete-Worktree.ps1
REM Usage: Delete-Worktree.cmd -Pattern <pattern> [-Force] [-KeepBranch]
setlocal
for /f "delims=" %%i in ('git rev-parse --show-toplevel 2^>nul') do set REPO_ROOT=%%i
if "%REPO_ROOT%"=="" (
    echo Error: Not inside a Git repository.
    exit /b 1
)
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\.github\skills\worktree-manager\scripts\Delete-Worktree.ps1" %*
