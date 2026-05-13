# AI issue triage (v1, dry-run)

A complementary triage agent for the **microsoft/WSL** GitHub repository. Reads
newly-opened issues, asks an LLM via [GitHub Models][gh-models] to classify
them, and posts a single collapsible maintainer-facing comment with:

* a 1–3 sentence plain-English summary,
* a suggested issue type (`bug`, `feature`, `question`, …),
* suggested component labels (e.g. `network`, `msix`, `GPU`),
* missing bug-template fields (Windows version, repro steps, …),
* up to ~5 possible duplicate issues.

This is **dry-run only**. The agent never applies labels and never changes
issue state. It is purely additive to the existing rule-based [`wti`][wti]
pipeline driven by [`triage/config.yml`](../config.yml).

## Files

| Path | Purpose |
|---|---|
| `triage/ai/ai_triage.py` | The Python script. Reads the issue, fetches duplicate candidates, calls `gh models run`, validates the output, upserts the comment. |
| `triage/ai/prompt.md` | The system+user prompt. The script substitutes `{{ISSUE_NUMBER}}`, `{{ISSUE_TITLE}}`, `{{ISSUE_BODY}}`, `{{CANDIDATES_JSON}}`. |
| `.github/workflows/ai_triage.yml` | The Actions workflow. Triggered on `issues.opened` and `workflow_dispatch`. |

## How to run locally

Prerequisites:

* Python 3.10+ (the script uses `list[str]` style annotations).
* `gh` CLI authenticated with at least `repo` and `read:user` scopes.
* The `gh-models` extension: `gh extension install github/gh-models`.

```bash
# Dry-run: print the rendered comment to stdout, do not post anything.
python triage/ai/ai_triage.py --issue 40488 --dry-run

# Force a re-run even if the input-sha marker says nothing changed.
python triage/ai/ai_triage.py --issue 40488 --dry-run --force

# Use a different GitHub Models model.
python triage/ai/ai_triage.py --issue 40488 --dry-run --model openai/gpt-4.1-mini

# Or via env var (matches the workflow):
AI_TRIAGE_MODEL=openai/gpt-4.1-mini python triage/ai/ai_triage.py --issue 40488 --dry-run
```

When run **without** `--dry-run`, the script will upsert a comment on the issue.
Don't do this against the live repo from a developer machine unless you're
deliberately testing — the workflow is the intended posting path.

## Skip rules

The agent does not run for issues where any of these is true:

* the issue is closed or locked,
* the author is a bot (`type == "Bot"` or login matches `*[bot]`),
* the author's `author_association` is `OWNER`, `MEMBER`, or `COLLABORATOR`
  (maintainer-authored issues don't need this triage),
* the body is shorter than 50 characters (likely empty or spam),
* the issue's input hash already matches the marker on an existing comment
  (use `--force` to override).

## Idempotency

Each posted comment includes a hidden marker:

```html
<!-- ai-triage:v1 input-sha=<hex> prompt-sha=<hex> -->
```

`input-sha` is computed over `(title, body, prompt-version)`. `prompt-sha` is
computed over the prompt template content. Re-runs that produce the same
hashes are skipped. After the model call, the script re-fetches the issue and
recomputes the hash — if it changed during the call, the run is aborted so a
slow run never overwrites a newer one.

Bumping `PROMPT_VERSION` in `ai_triage.py` (or editing `prompt.md`) invalidates
existing markers and forces the next run to re-post.

## Untrusted-input hardening

The model is treated as an untrusted text generator:

* JSON output is validated against a strict schema; any deviation aborts
  silently (no comment posted).
* `component_labels` are intersected with a hardcoded allowlist **and** the
  live `gh label list` for the repo.
* `duplicate_candidate_numbers` are intersected with the candidate set we
  pre-fetched via `gh search issues` — the model cannot invent issue numbers.
* The maintainer summary is HTML-escaped and run through a sanitizer that
  strips Markdown links, raw URLs, code fences, and defangs `@mentions` with
  a zero-width space.
* The prompt sent to the model contains only the issue title and body — never
  any comments. This means the model can never see (and therefore can never
  summarize) its own prior `<!-- ai-triage:v1 -->` comment, even on re-runs.

The prompt itself includes a hard rule telling the model to ignore
instructions inside the issue body.

## Failure mode

Silent. Any model error, JSON-parse failure, schema violation, rate-limit, or
post failure causes the script to log to stderr and exit 0 with no comment.
The workflow run shows the failure for maintainers; users see nothing.

## Cost / abuse posture

* `concurrency: cancel-in-progress` per issue prevents pile-ups on rapid edits.
* The body is truncated to 8000 characters before prompting.
* Duplicate retrieval is capped to ~15 candidates.
* The trigger is `issues.opened` only in v1 (no `edited`, no comment events).

If GitHub Models quota becomes a concern, mitigations to consider:

* tighten the body-length floor,
* add an author reputation prefilter (e.g. require N prior comments),
* widen the body truncation cap downward,
* downgrade to a smaller model.

## Graduation plan (v2 and beyond)

v1 deliberately does **not** apply labels. Before turning that on:

1. Run v1 in dry-run for a sustained period; spot-check a sample.
2. Compare suggested labels to what maintainers actually applied.
3. Pick a per-label confidence/calibration threshold.
4. Auto-apply only the safest labels first (suggested order: component labels
   that maintainers agree with most often). Type labels and any process labels
   (`needs-author-feedback`, `duplicate`, …) stay maintainer-only.

Other v2 candidates:

* Trigger on `issues.edited` with throttling.
* Trigger on first author comment to refresh the summary.
* Embed-based duplicate retrieval instead of keyword search.
* Cross-reference the diagnostic findings from `wti` to enrich the summary.

## Relationship to wti

`wti` (rule-based, runs from `new_issue.yml` / `new_issue_comment.yml` /
`issue_edited.yml`) is the existing pipeline. It excels at parsing attached
ETL log files against known signatures, applying tags like `init-crash` /
`network`, and posting canned remediation messages.

This AI agent is **complementary**, not a replacement. It works on the issue
prose. The two run independently and do not share state.

[gh-models]: https://github.com/github/gh-models
[wti]: https://github.com/OneBlue/wti
