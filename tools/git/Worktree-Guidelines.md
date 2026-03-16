# Git Worktree Helper Scripts

This folder contains convenience wrapper scripts for human users. The **master implementation** lives in the self-contained skill at `.github/skills/worktree-manager/`.

## Architecture

```
.github/skills/worktree-manager/       # Master source (self-contained skill)
├── SKILL.md                           # Skill documentation
└── scripts/
    ├── WorktreeLib.ps1                # Shared helpers (master)
    ├── New-WorktreeFromBranch.ps1     # Implementation
    ├── New-WorktreeFromIssue.ps1      # Implementation
    └── Delete-Worktree.ps1            # Implementation

tools/git/                             # Human-friendly wrappers
├── New-WorktreeFromBranch.cmd         # Wrapper -> skill
├── New-WorktreeFromIssue.cmd          # Wrapper -> skill
├── Delete-Worktree.cmd                # Wrapper -> skill
└── Worktree-Guidelines.md             # This file
```

## Why This Structure?

- **Self-contained skill**: The `.github/skills/worktree-manager/` folder is portable and works with GitHub Copilot agent skills
- **Human convenience**: The `tools/git/` wrappers provide familiar paths for developers
- **Single source of truth**: All logic lives in the skill folder; wrappers just delegate

## Why Worktrees?

Git worktrees let you have several checked-out branches sharing a single `.git` object store. Benefits:
- Fast context switching: no re-clone, no duplicate large binary/object downloads
- Lower disk usage versus multiple full clones
- Keeps each change isolated in its own folder so you can run builds/tests independently
- Enables working in parallel with branches created using GitHub Copilot assistance or by Copilot agents while the main clone stays clean

Recommended: keep active parallel worktrees to **≤ 3** per developer to reduce cognitive load and avoid excessive incremental build invalidations.

## Scripts Overview

| Script | Purpose |
|--------|---------|
| `New-WorktreeFromBranch.cmd` | Create/reuse a worktree for an existing local or remote (origin) branch |
| `New-WorktreeFromIssue.cmd` | Start a new work item branch from a base (default `origin/main`) using naming `workitem/<number>-<slug>` |
| `Delete-Worktree.cmd` | Remove a worktree and optionally its local branch |

## Typical Flows

### 1. Create from an existing or remote branch
```cmd
New-WorktreeFromBranch.cmd -Branch origin/feature/new-ui
```
Fetches if needed and creates a tracking branch if missing, then creates/reuses the worktree.

### 2. Start a new work item branch (Azure DevOps)
```cmd
New-WorktreeFromIssue.cmd -Number 1234 -Title "Crash on launch"
```
Creates branch `workitem/1234-crash-on-launch` off the auto-detected base (or explicit `-Base`), then worktree.
The base is auto-detected: `upstream/master` in a fork setup, `origin/master` in a direct clone.

### 3. Delete a worktree when done
```cmd
Delete-Worktree.cmd -Pattern feature/perf-tweak
```
If only one match, removes the worktree directory. Add `-Force` to discard local changes. Use `-KeepBranch` if you still need the branch.

## Naming & Locations

- Worktrees are created as sibling folders of the repo root (e.g., `WindowsAppSDK` + `WindowsAppSDK-ab12`), using a hash/short pattern to avoid collisions
- Work item branches: `workitem/<number>` or `workitem/<number>-<slug>`

## Best Practices

- Keep ≤ 3 active parallel worktrees (e.g., main dev, a long-lived feature, a quick fix / experiment) plus the root clone
- Delete stale worktrees early; each adds file watchers & potential incremental build churn
- Avoid editing the same file across multiple worktrees simultaneously to reduce merge friction
- Run `git fetch --all --prune` periodically in the primary repo, not in every worktree

## Troubleshooting

| Symptom | Hint |
|---------|------|
| Cannot lock ref *.lock | Stale lock: run `git worktree prune` or manually delete the `.lock` file then retry |
| Worktree already exists error | Use `git worktree list` to locate existing path; open that folder instead of creating a duplicate |
| Local branch missing for remote | Use `git branch --track <name> origin/<name>` then re-run the branch script |

## Security & Safety Notes

- Scripts avoid force-deleting unless you pass `-Force` (Delete script)
- No network credentials are stored; they rely on your existing Git credential helper

---
**Note**: For the full skill documentation, see [.github/skills/worktree-manager/SKILL.md](../../.github/skills/worktree-manager/SKILL.md).
