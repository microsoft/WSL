---
description: 'Best practices for WSL Azure DevOps pipeline YAML files'
applyTo: '.pipelines/**/*.yml'
---

# Azure DevOps Pipeline YAML — WSL

WSL CI/CD pipelines live under `.pipelines/` and use Azure DevOps (not GitHub Actions for builds). The pipelines handle PR validation, nightly builds, release builds, and localization through OneBranch.

## Pipeline Inventory

| Pipeline | Purpose |
|----------|---------|
| `wsl-build-pr.yml` / `wsl-build-pr-onebranch.yml` | PR validation builds |
| `wsl-build-nightly-onebranch.yml` | Nightly builds |
| `wsl-build-nightly-localization.yml` | Nightly localization sync |
| `wsl-build-release-onebranch.yml` | Release builds |
| `wsl-build-notice.yml` | NOTICE file generation |
| `build-stage.yml` | Shared build stage template |
| `test-stage.yml` / `test-job.yml` | Shared test stage/job templates |
| `nuget-stage.yml` | NuGet package stage template |
| `flight-stage.yml` | Flighting stage template |

## General Guidelines

- Use YAML syntax consistently with 2-space indentation.
- Always include meaningful `displayName` values for stages, jobs, and steps.
- Use templates (`build-stage.yml`, `test-stage.yml`, etc.) for reusable pipeline components.
- Keep pipeline files focused and modular — split stages into separate template files.

## WSL-Specific Build Considerations

- WSL builds require Windows agents with Visual Studio and Windows SDK 26100.
- Builds target both x64 and ARM64 — use matrix strategies or separate jobs.
- Full builds take 20–45 minutes; set appropriate timeouts.
- NuGet restore uses a custom Azure DevOps feed (see `nuget.config`).
- MSIX and MSI packaging are part of the build pipeline.

## Variable and Parameter Management

- Use variable groups for shared configuration across pipelines.
- Secure sensitive variables and mark them as secrets.
- Use runtime parameters for flexible pipeline execution (e.g., build configuration, target platform).

## Testing Integration

- Tests use the TAEF framework and require a full build before execution.
- Full test suite takes 30–60 minutes — set timeout to 90+ minutes.
- Tests require Administrator privileges on the agent.
- Publish test results in VSTest format.

## Branch and Trigger Strategy

- Use path filters to trigger builds only when relevant files change (exclude `doc/`, `README.md`).
- Configure PR triggers for code validation.
- Use scheduled triggers for nightly builds and localization syncs.

## Common Anti-Patterns to Avoid

- Hardcoding sensitive values directly in YAML files.
- Using overly broad triggers that cause unnecessary builds.
- Not using proper naming conventions for stages and jobs.
- Creating monolithic pipelines instead of using template composition.
