---
agent: 'agent'
description: 'Generate a WSL-ready pull request description from the local diff'
---

# Generate PR Summary

**Goal:** Produce a ready-to-paste PR title and description that follows WSL conventions by comparing the current branch against a user-selected target branch.

**Repo guardrails:**
- Treat `.github/pull_request_template.md` as the single source of truth; load it at runtime instead of embedding hardcoded content in this prompt.
- Preserve section order from the template but only surface checklist lines that are relevant for the detected changes, filling them with `[x]`/`[ ]` as appropriate.
- Cite touched paths with inline backticks, matching the guidance in `.github/copilot-instructions.md`.
- Call out validation explicitly: list automated checks run (for example TAEF, Linux unit tests, `mkdocs build -f doc/mkdocs.yml`, or `python3 distributions/validate.py`) or state why they are not applicable.
- Use WSL terminology when describing impacted components, such as `wsl.exe`, `wslservice`, `wslhost`, `init`, `gns`, `plan9`, `relay`, `drvfs`, `wslsettings`, `doc/`, or `distributions/`.

**Workflow:**
1. Determine the target branch from user context; default to `master` when no branch is supplied.
2. Run `git status --short` once to surface uncommitted files that may influence the summary.
3. Run `git diff <target-branch>...HEAD` a single time to review the detailed changes. Only when confidence stays low dig deeper with focused calls such as `git diff <target-branch>...HEAD -- <path>`.
4. From the diff, capture impacted WSL areas, key file changes, behavioral risks, migrations, and noteworthy edge cases. Use repo-specific component names when obvious (for example `service`, `wsl`, `init`, `gns`, `plan9`, `doc`, or `distributions`).
5. Confirm validation: list tests or validation commands executed with results, or state why they were skipped in line with repo guidance.
6. Load `.github/pull_request_template.md`, mirror its section order, and populate it with the gathered facts. Include only relevant checklist entries, such as tests, localization, dev docs, or docs repo follow-up, marking them `[x]/[ ]` and noting any intentional omissions as "N/A".
7. Present the filled template inside a fenced code block opened with ````markdown``` with no extra commentary so it is ready to paste into a PR, clearly flagging any placeholders that still need user input.
8. Prepend the PR title above the filled template, applying the Conventional Commit type/scope rules from `.github/prompts/create-commit-title.prompt.md`; pick the dominant WSL component from the diff and keep the title concise and imperative.
