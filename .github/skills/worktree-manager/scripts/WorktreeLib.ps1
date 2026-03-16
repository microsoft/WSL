# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

# WorktreeLib.ps1 - Shared helpers for worktree management
# This is the master source for the worktree-manager skill

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Info { param([string]$Message) Write-Host $Message -ForegroundColor Cyan }
function Warn { param([string]$Message) Write-Host $Message -ForegroundColor Yellow }
function Err  { param([string]$Message) Write-Host $Message -ForegroundColor Red }

<#
.SYNOPSIS
    Gets the root directory of the current Git repository.

.DESCRIPTION
    Uses git rev-parse to find the top-level directory of the current Git repository.
    Throws an exception if not inside a Git repository.

.OUTPUTS
    System.String. The absolute path to the repository root.

.EXAMPLE
    $root = Get-RepoRoot

.NOTES
    Requires Git to be installed and available in PATH.
#>
function Get-RepoRoot {
  $root = git rev-parse --show-toplevel 2>$null
  if (-not $root) { throw 'Not inside a git repository.' }
  return $root
}

<#
.SYNOPSIS
    Gets the parent directory where worktrees should be created.

.DESCRIPTION
    Returns the parent directory of the repository root, which is the default location
    for creating worktree directories as siblings of the main repository folder.

.PARAMETER RepoRoot
    The absolute path to the repository root directory.

.OUTPUTS
    System.String. The resolved path to the parent directory.

.EXAMPLE
    $basePath = Get-WorktreeBasePath -RepoRoot 'C:\repos\MyProject'

.NOTES
    Worktrees are created as siblings to the main repo folder (e.g., MyProject-ab12).
#>
function Get-WorktreeBasePath {
  param([string]$RepoRoot)
  # Always use parent of repo root (folder that contains the main repo directory)
  $parent = Split-Path -Parent $RepoRoot
  if (-not (Test-Path $parent)) { throw "Parent path for repo root not found: $parent" }
  return (Resolve-Path $parent).ProviderPath
}

<#
.SYNOPSIS
    Generates a short hash from a string for unique folder naming.

.DESCRIPTION
    Computes an MD5 hash of the input text and returns the first 4 hex characters.
    Used to create unique but short suffixes for worktree folder names.

.PARAMETER Text
    The input text to hash.

.OUTPUTS
    System.String. A 4-character hexadecimal string.

.EXAMPLE
    $hash = Get-ShortHashFromString -Text 'feature/login'
    # Returns something like 'ab12'

.NOTES
    MD5 is used for speed and collision avoidance, not for security.
#>
function Get-ShortHashFromString {
  param([Parameter(Mandatory)][string]$Text)
  $md5 = [System.Security.Cryptography.MD5]::Create()
  try {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $digest = $md5.ComputeHash($bytes)
    return -join ($digest[0..1] | ForEach-Object { $_.ToString('x2') })
  } finally { $md5.Dispose() }
}

<#
.SYNOPSIS
    Initializes Git submodules in a worktree if any exist.

.DESCRIPTION
    Checks if the repository has a .gitmodules file and, if so, syncs and updates
    all submodules recursively in the specified worktree path.

.PARAMETER RepoRoot
    The path to the main repository root to check for .gitmodules.

.PARAMETER WorktreePath
    The path to the worktree where submodules should be initialized.

.OUTPUTS
    System.Boolean. Returns $true if submodules were initialized, $false otherwise.

.EXAMPLE
    $initialized = Initialize-SubmodulesIfAny -RepoRoot 'C:\repos\MyProject' -WorktreePath 'C:\repos\MyProject-ab12'

.NOTES
    Submodule initialization can take time for large repositories.
#>
function Initialize-SubmodulesIfAny {
  param([string]$RepoRoot,[string]$WorktreePath)
  $hasGitmodules = Test-Path (Join-Path $RepoRoot '.gitmodules')
  if ($hasGitmodules) {
    git -C $WorktreePath submodule sync --recursive | Out-Null
    git -C $WorktreePath submodule update --init --recursive | Out-Null
    return $true
  }
  return $false
}

<#
.SYNOPSIS
    Creates or reuses a worktree for an existing local branch.

.DESCRIPTION
    Checks if a worktree already exists for the specified branch. If so, opens it in VS Code.
    Otherwise, creates a new worktree in a sibling folder and opens it in VS Code.

.PARAMETER Branch
    The name of the local branch to create a worktree for.

.PARAMETER VSCodeProfile
    The VS Code profile to use when opening the worktree folder.

.EXAMPLE
    New-WorktreeForExistingBranch -Branch 'feature/login' -VSCodeProfile 'Default'

.NOTES
    The branch must exist locally. Use New-WorktreeFromBranch.ps1 for remote branches.
#>
function New-WorktreeForExistingBranch {
  param(
    [Parameter(Mandatory)][string] $Branch,
    [Parameter(Mandatory)][string] $VSCodeProfile
  )
  $repoRoot = Get-RepoRoot
  git show-ref --verify --quiet "refs/heads/$Branch"; if ($LASTEXITCODE -ne 0) { throw "Branch '$Branch' does not exist locally." }

  # Detect existing worktree for this branch
  $entries = Get-WorktreeEntries
  $match = $entries | Where-Object { $_.Branch -eq $Branch } | Select-Object -First 1
  if ($match) {
    # If the branch is checked out in the main repo (not a secondary worktree),
    # detach main repo and create a proper worktree instead of reusing.
    $mainWorktreePath = (Resolve-Path $repoRoot).ProviderPath
    $matchPath = ($match.Path -replace '/', '\')
    $mainNormalized = ($mainWorktreePath -replace '/', '\')
    if ($matchPath -eq $mainNormalized) {
      Info "Branch '$Branch' is checked out in the main repo. Switching main repo to free the branch..."
      # Switch main repo to master/main so it's on a useful branch, not detached
      $defaultBase = Get-DefaultBaseRef
      $localDefault = ($defaultBase -replace '^[^/]+/', '')
      git show-ref --verify --quiet "refs/heads/$localDefault" 2>$null
      if ($LASTEXITCODE -eq 0 -and $localDefault -ne $Branch) {
        git checkout $localDefault 2>$null | Out-Null
        Info "Switched main repo to '$localDefault'."
      } else {
        git checkout --detach 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Cannot free branch '$Branch' from main repo. Checkout a different branch first." }
        Warn "Main repo is now in detached HEAD. Run 'git checkout $localDefault' when done."
      }
      # Now fall through to create the worktree below
    } else {
      Info "Reusing existing worktree for '$Branch': $($match.Path)"
      code --new-window "$($match.Path)" --profile "$VSCodeProfile" | Out-Null
      return
    }
  }

  $safeBranch = ($Branch -replace '[\\/:*?"<>|]','-')
  $hash = Get-ShortHashFromString -Text $safeBranch
  $folderName = "$(Split-Path -Leaf $repoRoot)-$hash"
  $base = Get-WorktreeBasePath -RepoRoot $repoRoot
  $folder = Join-Path $base $folderName
  $worktreeOutput = git worktree add $folder $Branch 2>&1
  if ($LASTEXITCODE -ne 0) { throw "Failed to create worktree at '$folder' for branch '$Branch': $worktreeOutput" }
  $inited = Initialize-SubmodulesIfAny -RepoRoot $repoRoot -WorktreePath $folder
  code --new-window "$folder" --profile "$VSCodeProfile" | Out-Null
  Info "Created worktree for branch '$Branch' at $folder."; if ($inited) { Info 'Submodules initialized.' }
}

<#
.SYNOPSIS
    Gets all Git worktree entries in the repository.

.DESCRIPTION
    Parses the output of 'git worktree list --porcelain' and returns structured objects
    containing the path and branch name for each worktree.

.OUTPUTS
    System.Object[]. Array of objects with Path and Branch properties.

.EXAMPLE
    $worktrees = Get-WorktreeEntries
    $worktrees | ForEach-Object { Write-Host "$($_.Branch) -> $($_.Path)" }

.NOTES
    Returns an empty array if no worktrees exist or if not in a Git repository.
#>
function Get-WorktreeEntries {
  # Returns objects with Path and Branch (branch without refs/heads/ prefix)
  $lines = git worktree list --porcelain 2>$null
  if (-not $lines) { return @() }
  $entries = @(); $current=@{ path=$null; branch=$null }
  foreach($l in $lines){
    if ($l -eq '') { if ($current.path -and $current.branch){ $entries += ,([pscustomobject]@{ Path=$current.path; Branch=($current.branch -replace '^refs/heads/','') }) }; $current=@{ path=$null; branch=$null }; continue }
    if ($l -like 'worktree *'){ $current.path = ($l -split ' ',2)[1] }
    elseif ($l -like 'branch *'){ $current.branch = ($l -split ' ',2)[1].Trim() }
    elseif ($l -eq 'detached'){ $current.branch = '(detached)' }
  }
  if ($current.path -and $current.branch){ $entries += ,([pscustomobject]@{ Path=$current.path; Branch=($current.branch -replace '^refs/heads/','') }) }
  return ($entries | Sort-Object Path,Branch -Unique)
}

<#
.SYNOPSIS
    Gets the upstream remote name for a branch.

.DESCRIPTION
    Returns the remote name (e.g., 'origin') if the branch has an upstream configured,
    or $null if no upstream is set.

.PARAMETER Branch
    The local branch name to check.

.OUTPUTS
    System.String or $null. The remote name if configured, otherwise $null.

.EXAMPLE
    $remote = Get-BranchUpstreamRemote -Branch 'main'
    # Returns 'origin' if main tracks origin/main

.NOTES
    Requires the branch to exist locally.
#>
function Get-BranchUpstreamRemote {
  param([Parameter(Mandatory)][string]$Branch)
  # Returns remote name if branch has an upstream, else $null
  $ref = git rev-parse --abbrev-ref --symbolic-full-name "$Branch@{upstream}" 2>$null
  if ($LASTEXITCODE -ne 0 -or -not $ref) { return $null }
  if ($ref -match '^(?<remote>[^/]+)/.+$') { return $Matches.remote }
  return $null
}

<#
.SYNOPSIS
    Displays common manual Git commands for worktree troubleshooting.

.DESCRIPTION
    Prints a formatted list of helpful Git commands that users can run manually
    when automated worktree operations fail or need verification.

.EXAMPLE
    Show-CommonManualStepsFooter

.NOTES
    Called at the end of help displays and error recovery messages.
#>
function Show-CommonManualStepsFooter {
  Info '--- Common Manual Steps ---'
  Info 'List worktree:      git worktree list --porcelain'
  Info 'List branches:       git branch -vv'
  Info 'List remotes:        git remote -v'
  Info 'Prune worktree:     git worktree prune'
  Info 'Remove worktree dir: Remove-Item -Recurse -Force <path>'
  Info 'Reset branch:        git reset --hard HEAD'
}

<#
.SYNOPSIS
    Displays a summary of worktree operations and current state.

.DESCRIPTION
    Prints information about the current branch, worktree path, all existing worktrees,
    and configured remotes. Used after worktree operations to confirm success.

.PARAMETER CurrentBranch
    The branch name associated with the current operation (optional).

.PARAMETER WorktreePath
    The path to the worktree that was created or modified (optional).

.EXAMPLE
    Show-WorktreeExecutionSummary -CurrentBranch 'feature/login' -WorktreePath 'C:\repos\MyProject-ab12'

.NOTES
    Shows all existing worktrees regardless of which one was just created.
#>
function Show-WorktreeExecutionSummary {
  param(
    [string]$CurrentBranch,
    [string]$WorktreePath
  )
  Info '--- Summary ---'
  if ($CurrentBranch) { Info "Branch:        $CurrentBranch" }
  if ($WorktreePath) { Info "Worktree path: $WorktreePath" }
  $entries = Get-WorktreeEntries
  if ($entries.Count -gt 0) {
    Info 'Existing worktrees:'
    $entries | ForEach-Object { Info ("  {0} -> {1}" -f $_.Branch,$_.Path) }
  }
  Info 'Remotes:'
  git remote -v 2>$null | Sort-Object | Get-Unique | ForEach-Object { Info "  $_" }
}

<#
.SYNOPSIS
    Displays embedded help from a script file's comment block.

.DESCRIPTION
    Reads a PowerShell script file and extracts the help text from a special
    embedded help block (delimited by angle-bracket-hash-exclamation markers),
    then displays it along with common manual steps.

.PARAMETER ScriptPath
    The full path to the script file containing embedded help.

.EXAMPLE
    Show-FileEmbeddedHelp -ScriptPath 'C:\scripts\New-WorktreeFromBranch.ps1'

.NOTES
    The script must contain an embedded help block with the special marker syntax.
#>
function Show-FileEmbeddedHelp {
  param([string]$ScriptPath)
  if (-not (Test-Path $ScriptPath)) { throw "Cannot load help; file missing: $ScriptPath" }
  $content = Get-Content -LiteralPath $ScriptPath -ErrorAction Stop
  $inBlock=$false
  foreach($line in $content){
    if ($line -match '^<#!') { $inBlock=$true; continue }
    if ($line -match '#>$') { break }
    if ($inBlock) { Write-Host $line }
  }
  Show-CommonManualStepsFooter
}

<#
.SYNOPSIS
    Sets or updates the upstream tracking branch for a local branch.

.DESCRIPTION
    Configures the upstream remote and branch for a local branch. If already set to
    the specified upstream, takes no action. If set to a different upstream, updates it.

.PARAMETER LocalBranch
    The local branch name to configure.

.PARAMETER RemoteName
    The name of the remote (e.g., 'origin').

.PARAMETER RemoteBranchPath
    The branch path on the remote (e.g., 'main' for origin/main).

.EXAMPLE
    Set-BranchUpstream -LocalBranch 'feature/login' -RemoteName 'origin' -RemoteBranchPath 'feature/login'

.NOTES
    Displays a warning if the upstream cannot be set automatically.
#>
function Set-BranchUpstream {
  param(
    [Parameter(Mandatory)][string]$LocalBranch,
    [Parameter(Mandatory)][string]$RemoteName,
    [Parameter(Mandatory)][string]$RemoteBranchPath
  )
  $current = git rev-parse --abbrev-ref --symbolic-full-name "$LocalBranch@{upstream}" 2>$null
  if (-not $current) {
    Info "Setting upstream: $LocalBranch -> $RemoteName/$RemoteBranchPath"
    git branch --set-upstream-to "$RemoteName/$RemoteBranchPath" $LocalBranch 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Warn "Failed to set upstream automatically. Run: git branch --set-upstream-to $RemoteName/$RemoteBranchPath $LocalBranch" }
    return
  }
  if ($current -ne "$RemoteName/$RemoteBranchPath") {
    Warn "Upstream mismatch ($current != $RemoteName/$RemoteBranchPath); updating..."
    git branch --set-upstream-to "$RemoteName/$RemoteBranchPath" $LocalBranch 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Warn "Could not update upstream; manual fix: git branch --set-upstream-to $RemoteName/$RemoteBranchPath $LocalBranch" } else { Info 'Upstream corrected.' }
  } else { Info "Upstream already: $current" }
}

<#
.SYNOPSIS
    Returns an ordered list of remote names, preferring 'upstream' then 'origin'.

.DESCRIPTION
    Queries 'git remote' and returns all configured remotes sorted so that
    'upstream' (if present) comes first, then 'origin', then any others.
    Works for both fork setups (origin=fork, upstream=source) and direct
    clones (origin=source, no upstream).

.OUTPUTS
    System.String[]. Ordered list of remote names.

.EXAMPLE
    $remotes = Get-OrderedRemotes
    # Fork:   @('upstream', 'origin')
    # Direct: @('origin')
#>
function Get-OrderedRemotes {
  $all = @(git remote 2>$null)
  if (-not $all) { return @() }
  $ordered = @()
  if ($all -contains 'upstream') { $ordered += 'upstream' }
  if ($all -contains 'origin')   { $ordered += 'origin' }
  $ordered += ($all | Where-Object { $_ -ne 'upstream' -and $_ -ne 'origin' })
  return $ordered
}

<#
.SYNOPSIS
    Finds the best default base ref for new branches.

.DESCRIPTION
    Checks remote-tracking refs in this order: upstream/master, upstream/main,
    origin/master, origin/main. If none of those refs exist locally, it tries to
    resolve origin/HEAD and falls back to origin/main.

.OUTPUTS
    System.String. A remote ref such as 'upstream/master' or 'origin/main'.

.EXAMPLE
    $base = Get-DefaultBaseRef
    # Fork:   'upstream/master'
    # Direct: 'origin/main' or 'origin/master'
#>
function Get-DefaultBaseRef {
  foreach ($ref in @('upstream/master', 'upstream/main', 'origin/master', 'origin/main')) {
    git show-ref --verify --quiet "refs/remotes/$ref" 2>$null
    if ($LASTEXITCODE -eq 0) { return $ref }
  }

  $originHead = git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>$null
  if ($LASTEXITCODE -eq 0 -and $originHead) { return $originHead.Trim() }

  return 'origin/main'
}

<#
.SYNOPSIS
    Finds a branch across all remotes and returns the first matching remote ref.

.DESCRIPTION
    Searches all configured remotes (upstream first, then origin, then others)
    for a branch name. Returns the first matching remote/branch ref, or $null.

.PARAMETER BranchName
    The branch name to search for (without remote prefix).

.OUTPUTS
    System.String or $null. E.g., 'upstream/feature/foo' or 'origin/feature/foo'.

.EXAMPLE
    $ref = Find-BranchOnRemotes -BranchName 'feature/login'
#>
function Find-BranchOnRemotes {
  param([Parameter(Mandatory)][string]$BranchName)
  $remotes = Get-OrderedRemotes
  foreach ($remote in $remotes) {
    git show-ref --verify --quiet "refs/remotes/$remote/$BranchName" 2>$null
    if ($LASTEXITCODE -eq 0) { return "$remote/$BranchName" }
  }
  return $null
}
