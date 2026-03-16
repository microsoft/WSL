---
name: worktree-manager
description: Create and manage Git worktrees for parallel development workflows. Use when multiple self-contained issues should NOT be fixed in a single branch, when human-Copilot iteration requires isolated environments with separate chat history and commits, or when parallel work items need independent build/test results. Triggers on requests involving branch isolation, work item separation, parallel development, or avoiding messy branch switching.
---

# Git Worktree Manager

This skill provides helper scripts and workflows for creating and managing Git worktrees, enabling parallel development across multiple branches without cloning the repository multiple times.

## When to Use This Skill

**Primary triggers** — Create a new worktree when:

1. **Multiple self-contained issues exist** — Different problems should NOT be fixed together in a single branch. Each issue deserves its own branch with isolated commits, separate PR, and independent review.

2. **Human-Copilot iteration is needed** — Step-by-step collaboration requires an isolated environment where:
   - Each worktree has its own Copilot chat history and context
   - Plan and TODO progress stays scoped to that specific issue
   - Commits are atomic and traceable to one problem

3. **Parallel work avoids branch-switching chaos** — Instead of constantly switching branches (which mixes up build artifacts, test results, and Copilot context), worktrees let you:
   - Keep each VS Code window focused on one task
   - Run builds/tests independently without cross-contamination
   - Resume work instantly without stashing or context loss

**Anti-patterns** — Do NOT use worktrees when:
- A quick one-liner fix can be committed directly
- Changes are tightly coupled and belong in the same PR
- You're just reading code (no commits planned)

## Prerequisites

- Git installed and configured
- PowerShell 5.1+ on Windows (PowerShell 7+ recommended) or PowerShell Core cross-platform
- VS Code installed (for automatic workspace opening)
- Repository must be a Git repository (not inside a worktree already)

## Available Scripts

This skill provides focused scripts for each worktree operation.

### New-WorktreeFromBranch.ps1

Create or reuse a worktree for an existing local or remote branch.

**Location**: `./scripts/New-WorktreeFromBranch.ps1`

```powershell
./scripts/New-WorktreeFromBranch.ps1 -Branch <branch-name> [-NoFetch]
```

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `-Branch` | Yes | - | Branch name (local or `origin/<name>` form) |
| `-NoFetch` | No | `$false` | Skip remote fetch if branch missing locally |

**Examples**:
```powershell
# From a local or remote branch
./scripts/New-WorktreeFromBranch.ps1 -Branch feature/login

# From origin remote (normalizes automatically)
./scripts/New-WorktreeFromBranch.ps1 -Branch origin/bugfix/nullref
```

---

### New-WorktreeFromIssue.ps1

Start a new work item branch with consistent naming and create a worktree for it.

**Location**: `./scripts/New-WorktreeFromIssue.ps1`

```powershell
./scripts/New-WorktreeFromIssue.ps1 -Number <workitem-number> [-Title <description>] [-Base <ref>]
```

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `-Number` | Yes | - | Azure DevOps work item number for branch naming |
| `-Title` | No | - | Descriptive title (slugified into branch name) |
| `-Base` | No | Auto-detected (`upstream/master` > `upstream/main` > `origin/master` > `origin/main`, then `origin/HEAD` when available) | Base ref to branch from |

**Examples**:
```powershell
# Work item branch with title
./scripts/New-WorktreeFromIssue.ps1 -Number 1234 -Title "Crash on launch"
# Creates: workitem/1234-crash-on-launch

# Work item branch from different base
./scripts/New-WorktreeFromIssue.ps1 -Number 42 -Base origin/develop

# Simple work item branch (no title slug)
./scripts/New-WorktreeFromIssue.ps1 -Number 999
# Creates: workitem/999
```

---

### Delete-Worktree.ps1

Remove a worktree and optionally its associated branch.

**Location**: `./scripts/Delete-Worktree.ps1`

```powershell
./scripts/Delete-Worktree.ps1 -Pattern <pattern> [-Force] [-KeepBranch]
```

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `-Pattern` | Yes | - | Partial branch name or path to match |
| `-Force` | No | `$false` | Force removal even with uncommitted changes |
| `-KeepBranch` | No | `$false` | Don't delete the local branch |

**Examples**:
```powershell
# Delete worktree by branch pattern
./scripts/Delete-Worktree.ps1 -Pattern feature/perf-tweak

# Force delete with uncommitted changes
./scripts/Delete-Worktree.ps1 -Pattern workitem/1234 -Force

# Delete worktree but keep the branch
./scripts/Delete-Worktree.ps1 -Pattern feature/ui -KeepBranch
```

---

### List Worktrees

Use Git directly to list worktrees:

```powershell
git worktree list --porcelain
```

## Step-by-Step Workflows

### Workflow 1: Work on an Azure DevOps Work Item

1. Note the work item number and title
2. Create work item branch and worktree:
   ```powershell
   ./scripts/New-WorktreeFromIssue.ps1 -Number 1234 -Title "Fix null reference"
   ```
3. Build, test, and develop as normal
4. Commit, push, and open PR
5. Delete worktree when PR is merged:
   ```powershell
   ./scripts/Delete-Worktree.ps1 -Pattern workitem/1234
   ```

### Workflow 2: Parallel Feature Development

1. Create worktrees for each feature:
   ```powershell
   ./scripts/New-WorktreeFromBranch.ps1 -Branch feature/login
   ./scripts/New-WorktreeFromBranch.ps1 -Branch feature/dashboard
   ```
2. Work on each independently (different VS Code windows)
3. Keep ≤ 3 active worktrees to manage cognitive load

## Worktree Naming & Locations

| Source | Local Branch Name | Worktree Folder |
|--------|-------------------|-----------------|
| Local/remote branch | Same as branch | `<RepoName>-<hash>/` |
| Work item | `workitem/<number>-<slug>` | `<RepoName>-<hash>/` |

Worktrees are created as **sibling folders** to the repository root (e.g., `MyRepo/` and `MyRepo-ab12/`).

## Best Practices

- **Keep ≤ 3 active worktrees** per developer to reduce cognitive load
- **Delete stale worktrees early** — each adds file watchers and build churn
- **Avoid editing the same file** across multiple worktrees simultaneously
- **Run `git fetch --all --prune`** periodically in the primary repo, not every worktree
- **Use targeted builds** inside worktrees instead of full builds

## Troubleshooting

| Symptom | Solution |
|---------|----------|
| Cannot lock ref (*.lock error) | Run `git worktree prune` or delete stale `.lock` file manually |
| Worktree already exists | Use `git worktree list` to find existing path; reuse that folder |
| Local branch missing for remote | Run `git branch --track <name> origin/<name>` then retry |
| Submodules not initialized | Run `git submodule update --init --recursive` in the worktree |

## Common Commands Reference

```powershell
# List all worktrees
git worktree list --porcelain

# List local branches with tracking info
git branch -vv

# List remotes
git remote -v

# Prune stale worktree references
git worktree prune

# Force remove a worktree with uncommitted changes
git worktree remove --force <path>
```

## Security Notes

- Scripts do **not** store credentials — they rely on your existing Git credential helper
- No destructive operations without explicit `-Force` flag

## Script Locations

All implementation is self-contained within this skill folder:

**Scripts** (master source):
- [New-WorktreeFromBranch.ps1](./scripts/New-WorktreeFromBranch.ps1) — create worktree from branch
- [New-WorktreeFromIssue.ps1](./scripts/New-WorktreeFromIssue.ps1) — create worktree for work item
- [Delete-Worktree.ps1](./scripts/Delete-Worktree.ps1) — remove worktree and branch
- [WorktreeLib.ps1](./scripts/WorktreeLib.ps1) — shared helper functions

**Human-friendly wrappers** (in `tools/git/`):
- `tools/git/New-WorktreeFromBranch.cmd` — delegates to skill's master script
- `tools/git/New-WorktreeFromIssue.cmd` — delegates to skill's master script  
- `tools/git/Delete-Worktree.cmd` — delegates to skill's master script
- `tools/git/Worktree-Guidelines.md` — quick reference for developers
