---
agent: 'agent'
description: 'Generate an 80-character git commit title for the local diff'
---

# Generate Commit Title

## Purpose
Provide a single-line, ready-to-paste git commit title (<= 80 characters) that reflects the most important local changes since `HEAD`.

## Input to collect
- Run exactly one command to view the local diff:
  ```@terminal
  git diff HEAD
  ```

## How to decide the title
1. From the diff, find the dominant area (e.g., `src/windows/service/`, `src/windows/wsl/`, `src/linux/init/`, `src/shared/`, `doc/`, or `distributions/`) and the change type (bug fix, docs update, config tweak).
2. Draft an imperative, plain-ASCII title that:
   - Mentions the primary WSL component when obvious (e.g., `service:` or `docs:`)
   - Stays within 80 characters and has no trailing punctuation

## Final output
- Reply with only the commit title on a single line—no extra text.

## PR title convention (when asked)
Use Conventional Commits style:

`<type>(<scope>): <summary>`

**Allowed types**
- feat, fix, docs, refactor, perf, test, build, ci, chore

**Scope rules**
- Use a short, WSL-focused scope (one word preferred when practical). Common scopes:
  - `service`, `wsl`, `wslhost`, `wslrelay`, `wslg`, `libwsl`, `wslsettings`
  - `init`, `gns`, `plan9`, `localhost`, `relay`, `session-leader`, `drvfs`, `binfmt`
  - `docs`, `distributions`, `pipelines`, `build`, `test`, `config`
- If unclear, pick the closest component or subsystem; omit only if unavoidable

**Summary rules**
- Imperative, present tense (“add”, “update”, “remove”, “fix”)
- Keep it <= 72 characters when possible; be specific, avoid “misc changes”

**Examples**
- `fix(service): guard VM teardown after launch failure`
- `feat(init): add validation for systemd boot config`
- `docs(docs): clarify debug-shell troubleshooting steps`
- `build(distributions): validate modern distro metadata`
- `ci(pipelines): cache build artifacts for x64`
