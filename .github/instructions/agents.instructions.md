---
description: 'Guidelines for creating custom agent files for GitHub Copilot in the WSL repo'
applyTo: '**/*.agent.md'
---

# Custom Agent File Guidelines — WSL

Instructions for creating custom agent files that provide specialized expertise for WSL development tasks in GitHub Copilot.

## File Basics

- File format: Markdown with YAML frontmatter
- Naming: lowercase with hyphens (e.g., `wsl-build-helper.agent.md`)
- Location: `.github/agents/` directory
- Purpose: Define specialized agents with tailored expertise for WSL development

## Example Frontmatter

```yaml
---
description: 'Brief description of the agent purpose and capabilities'
# name: 'Agent Display Name'      # Optional
# tools: ['read', 'edit', 'search']  # Optional
# model: 'Claude Sonnet 4.5'      # Optional
---
```

### Core Properties

| Property | Required | Description |
|----------|----------|-------------|
| `description` | **Yes** | Concise purpose statement (50–150 chars) |
| `name` | No | Display name; defaults to filename |
| `tools` | No | Tool list; omit for all tools |
| `model` | Optional | AI model to use |
| `target` | No | `'vscode'` or `'github-copilot'` |
| `infer` | No | Auto-activate on context match (default: `true`) |
| `handoffs` | No | Workflow transitions to other agents (VS Code 1.106+) |

## Tool Configuration

### Standard Tool Aliases

| Alias | Description |
|-------|-------------|
| `execute` | Execute shell commands (PowerShell/Bash) |
| `read` | Read file contents |
| `edit` | Edit and modify files |
| `search` | Search for files or text |
| `agent` | Invoke other custom agents |
| `web` | Fetch web content and search |

### Tool Selection for WSL Agents

- **Build agents**: `['read', 'search', 'execute']` — need shell for cmake/msbuild
- **Code review agents**: `['read', 'search']` — read-only sufficient
- **Test agents**: `['read', 'search', 'execute']` — need shell for TAEF
- **Documentation agents**: `['read', 'edit', 'search']` — need edit for doc files

## Agent Prompt Structure

1. **Agent Identity and Role**: Primary expertise
2. **Core Responsibilities**: Specific tasks
3. **WSL Context**: Key paths, build system, platform constraints
4. **Guidelines and Constraints**: What to do/avoid
5. **Output Expectations**: Expected format

## Handoffs (VS Code 1.106+)

```yaml
handoffs:
  - label: Run Tests
    agent: wsl-test-helper
    prompt: 'Run the full test suite on the build we just completed.'
    send: false
```

| Property | Required | Description |
|----------|----------|-------------|
| `label` | Yes | Button text shown in chat |
| `agent` | Yes | Target agent identifier |
| `prompt` | No | Pre-filled prompt for the target agent |
| `send` | No | Auto-submit (default: `false`) |

## Sub-Agent Invocation

- Include `agent` in the orchestrator's `tools` list.
- Pass paths and identifiers, not full file contents.
- Sub-agents inherit tool ceiling from parent's `tools` list.
- Limit to 5–10 sequential steps; use a single agent for bulk work.
