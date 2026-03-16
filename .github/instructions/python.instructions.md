---
description: 'Guidelines for WSL Python validation and DevOps scripts'
applyTo: '**/*.py'
---

# Python Scripts — WSL

Python in this repo is used exclusively for lightweight validation and DevOps tooling — no frameworks, no AI/ML, no web apps.

## Script Inventory

| Script | Purpose |
|--------|---------|
| `distributions/validate.py` | Validate distribution metadata (`DistributionInfo.json`) |
| `distributions/validate-modern.py` | Validate modern distribution support |
| `tools/devops/validate-localization.py` | Validate localization resource files (requires Windows build first) |
| `tools/devops/validate-copyright-headers.py` | Validate copyright headers across source files |
| `tools/devops/find-release.py` | Find release information |
| `tools/devops/create-release.py` | Create release artifacts |
| `tools/devops/create-change.py` | Create change records |
| `tools/test/gh-release-server.py` | Mock GitHub release server for testing |

## Guidelines

- Use Python 3.8+ compatible syntax.
- Keep scripts self-contained — prefer stdlib over external dependencies.
- Use `argparse` for CLI argument parsing.
- Print clear, actionable error messages on failure.
- Exit with non-zero status on failure (for CI integration).
- Validation scripts must be idempotent — safe to run multiple times.

## Known Caveats

- `validate-copyright-headers.py` reports issues in `_deps/` — expected, ignore.
- `validate-localization.py` requires `localization/strings/en-us/Resources.resw` (only after Windows build).
- `validate.py` may fail on Linux due to network restrictions accessing distribution URLs.
