# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

<#!
.SYNOPSIS
    Create (or reuse) a worktree for a new work item branch derived from a base ref.

.DESCRIPTION
    Composes a branch name as workitem/<number> or workitem/<number>-<slug> (slug from optional -Title).
    If the branch does not already exist, it is created from -Base (default origin/main). Then a
    worktree is created or reused.

.PARAMETER Number
    Azure DevOps work item number used to construct the branch name.

.PARAMETER Title
    Optional descriptive title; slug into the branch name.

.PARAMETER Base
    Base ref to branch from. Auto-detected if omitted: prefers upstream/master
    (fork setup) then origin/master (direct clone), then origin/main.

.PARAMETER VSCodeProfile
    VS Code profile to open (Default).

.EXAMPLE
    ./New-WorktreeFromIssue.ps1 -Number 1234 -Title "Crash on launch"

.EXAMPLE
    ./New-WorktreeFromIssue.ps1 -Number 42 -Base origin/develop

.NOTES
    Manual recovery:
        git fetch origin
        git checkout -b workitem/<num>-<slug> <base>
        git worktree add ../Repo-XX workitem/<num>-<slug>
        code ../Repo-XX
#>

[CmdletBinding()]
param(
    [int] $Number,
    [string] $Title,
    [string] $Base,
    [Alias('Profile')][string] $VSCodeProfile = 'Default',
    [switch] $Help
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/WorktreeLib.ps1"
$scriptPath = $MyInvocation.MyCommand.Path
if ($Help -or -not $Number) { Show-FileEmbeddedHelp -ScriptPath $scriptPath; return }

# Auto-detect base if not provided: upstream/master > origin/master > origin/main
if (-not $Base) { $Base = Get-DefaultBaseRef; Info "Auto-detected base: $Base" }

# Compose branch name
if ($Title) {
    $slug = ($Title -replace '[^\w\- ]','').ToLower() -replace ' +','-'
    $branch = "workitem/$Number-$slug"
} else {
    $branch = "workitem/$Number"
}

try {
    # Create branch if missing
    git show-ref --verify --quiet "refs/heads/$branch"
    if ($LASTEXITCODE -ne 0) {
        Info "Creating branch $branch from $Base"
        git branch $branch $Base 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to create branch $branch from $Base" }
    } else {
        Info "Branch $branch already exists locally." 
    }

    New-WorktreeForExistingBranch -Branch $branch -VSCodeProfile $VSCodeProfile
    $after = Get-WorktreeEntries | Where-Object { $_.Branch -eq $branch }
    $entry = $after | Select-Object -First 1
    $path = if ($entry) { $entry.Path } else { $null }
    Show-WorktreeExecutionSummary -CurrentBranch $branch -WorktreePath $path
    exit 0
} catch {
    Err "Error: $($_.Exception.Message)"
    Warn 'Manual steps:'
    Info "  git fetch origin"
    Info "  git checkout -b $branch $Base  (if branch missing)"
    Info "  git worktree add ../<Repo>-XX $branch"
    Info '  code ../<Repo>-XX'
    exit 1
}
