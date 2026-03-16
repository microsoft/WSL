# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

<#!
.SYNOPSIS
    Remove a Git worktree and optionally its associated branch.

.DESCRIPTION
    Finds a worktree by pattern match (branch name or path), removes the worktree directory,
    and optionally cleans up the local branch.

.PARAMETER Pattern
    Partial branch name or path pattern to match the worktree.

.PARAMETER Force
    Force removal even if there are uncommitted changes.

.PARAMETER KeepBranch
    Do not delete the local branch after removing the worktree.

.PARAMETER Help
    Show this help message.

.EXAMPLE
    ./Delete-Worktree.ps1 -Pattern feature/perf-tweak

.EXAMPLE
    ./Delete-Worktree.ps1 -Pattern workitem/1234 -Force

.NOTES
    Manual equivalent:
        git worktree list
        git worktree remove <path>
        git branch -d <branch>
        git worktree prune
#>

[CmdletBinding()]
param(
    [string] $Pattern,
    [switch] $Force,
    [switch] $KeepBranch,
    [switch] $Help
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/WorktreeLib.ps1"

if ($Help -or -not $Pattern) { Show-FileEmbeddedHelp -ScriptPath $MyInvocation.MyCommand.Path; return }

try {
    $repoRoot = Get-RepoRoot
    $entries = @(Get-WorktreeEntries)
    
    if ($entries.Count -eq 0) {
        Info "No worktrees found."
        return
    }
    
    # Find matching worktrees (by branch or path)
    $matchedEntries = @($entries | Where-Object { 
        $_.Branch -like "*$Pattern*" -or $_.Path -like "*$Pattern*" 
    })
    
    if ($matchedEntries.Count -eq 0) {
        Err "No worktree found matching pattern: $Pattern"
        Info "Available worktrees:"
        $entries | ForEach-Object { Info "  $($_.Branch) -> $($_.Path)" }
        exit 1
    }
    
    if ($matchedEntries.Count -gt 1) {
        Warn "Multiple worktrees match pattern '$Pattern'. Please be more specific:"
        $matchedEntries | ForEach-Object { Info "  $($_.Branch) -> $($_.Path)" }
        exit 1
    }
    
    $target = $matchedEntries[0]
    $worktreePath = $target.Path
    $branchName = $target.Branch
    
    # Check if this is the main worktree (repo root)
    if ((Resolve-Path $worktreePath -ErrorAction SilentlyContinue) -eq (Resolve-Path $repoRoot -ErrorAction SilentlyContinue)) {
        Err "Cannot remove the main worktree (repository root)."
        exit 1
    }
    
    Info "Removing worktree: $branchName -> $worktreePath"
    
    # Remove the worktree
    if ($Force) {
        $result = git worktree remove --force $worktreePath 2>&1
        if ($LASTEXITCODE -ne 0) {
            Err "Failed to force-remove worktree."
            Err $result
            exit 1
        }
    } else {
        $result = git worktree remove $worktreePath 2>&1
        if ($LASTEXITCODE -ne 0) {
            Err "Failed to remove worktree. Use -Force to discard uncommitted changes."
            Err $result
            exit 1
        }
    }
    
    if ($LASTEXITCODE -eq 0) {
        Info "Worktree removed: $worktreePath"
    }
    
    # Clean up branch if requested
    if (-not $KeepBranch) {
        # Check if branch still exists
        git show-ref --verify --quiet "refs/heads/$branchName"
        if ($LASTEXITCODE -eq 0) {
            Info "Deleting local branch: $branchName"
            if ($Force) {
                git branch -D $branchName 2>&1 | Out-Null
            } else {
                git branch -d $branchName 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    Warn "Could not delete branch (may have unmerged changes). Use -Force or delete manually:"
                    Warn "  git branch -D $branchName"
                }
            }
            if ($LASTEXITCODE -eq 0) {
                Info "Branch deleted: $branchName"
            }
        }
    } else {
        Info "Keeping branch: $branchName"
    }
    
    # Prune any stale worktree references
    git worktree prune 2>&1 | Out-Null
    
    Show-WorktreeExecutionSummary -CurrentBranch $null -WorktreePath $null
    Info "Done."
    exit 0
} catch {
    Err "Error: $($_.Exception.Message)"
    Warn 'Manual steps:'
    Info '  git worktree list'
    Info '  git worktree remove <path>'
    Info '  git branch -d <branch>'
    Info '  git worktree prune'
    exit 1
}
