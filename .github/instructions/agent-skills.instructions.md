---
description: 'Guidelines for creating Agent Skills for GitHub Copilot in the WSL repo'
applyTo: '.github/skills/**/SKILL.md'
---

# Agent Skills File Guidelines — WSL

Instructions for creating Agent Skills that enhance GitHub Copilot with WSL-specific capabilities.

## What Are Agent Skills?

Self-contained folders with instructions and bundled resources that teach AI agents specialized capabilities. Each skill lives in `.github/skills/<skill-name>/` and must contain a `SKILL.md` file.

Key characteristics:
- **Progressive loading**: Only loaded when relevant to the user's request
- **Resource-bundled**: Can include scripts, templates, examples
- **Portable**: Works across VS Code, Copilot CLI, and coding agent

## Required SKILL.md Format

### Frontmatter

```yaml
---
name: wsl-debugging
description: Toolkit for debugging WSL issues using ETL tracing, debug console, and log collection. Use when asked to diagnose WSL crashes, networking problems, filesystem issues, or collect diagnostic logs.
---
```

| Field | Required | Constraints |
|-------|----------|-------------|
| `name` | Yes | Lowercase, hyphens, max 64 chars |
| `description` | Yes | Capabilities + triggers + keywords, max 1024 chars |

### Description Best Practices

The `description` is the PRIMARY mechanism for skill discovery. Include:
1. **WHAT** the skill does
2. **WHEN** to use it (triggers, scenarios)
3. **Keywords** users might mention

## Bundling Resources

| Folder | Purpose | Example |
|--------|---------|---------|
| `scripts/` | Executable automation | `collect-traces.ps1` |
| `references/` | Documentation the agent reads | `etl-providers.md` |
| `templates/` | Starter code the agent modifies | `new-test.cpp` |

Reference with relative paths: `See [ETL guide](./references/etl-providers.md)`

## Content Guidelines

- Imperative mood: "Run", "Create", "Configure"
- Include exact commands with WSL-specific parameters
- Keep SKILL.md body under 500 lines; split into `references/`
- No hardcoded credentials or secrets
