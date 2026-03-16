---
description: 'Guidelines for creating prompt files for GitHub Copilot in the WSL repo'
applyTo: '**/*.prompt.md'
---

# Copilot Prompt Files Guidelines — WSL

Instructions for creating reusable prompt files that guide GitHub Copilot for WSL development tasks.

## Frontmatter

```yaml
---
description: 'A short description of the prompt outcome'
name: 'optional-slash-command-name'
agent: 'ask | edit | agent'
tools: ['list', 'of', 'tools']
---
```

| Field | Required | Description |
|-------|----------|-------------|
| `description` | Recommended | Short prompt description |
| `name` | Optional | Slash-command name (defaults to filename) |
| `agent` | Recommended | `ask`, `edit`, or `agent` |
| `tools` | Optional | Available tools list |

## File Naming and Placement

- Kebab-case filenames ending with `.prompt.md`
- Store under `.github/prompts/`
- Descriptive names: `build-wsl.prompt.md`, not `prompt1.prompt.md`

## Body Structure

- Start with `#` heading matching the intent
- Sections: Mission, Scope, Inputs, Workflow, Output Expectations, Validation
- Reference related prompts or instructions via relative links

## Input Handling

- Use `${input:variableName}` for required values
- Document fallback when context is missing
- Warn about destructive operations (MSI install, service restart)

## Style

- Direct, imperative sentences targeted at Copilot
- Reference WSL-specific paths and commands directly
- Keep sentences short and unambiguous
