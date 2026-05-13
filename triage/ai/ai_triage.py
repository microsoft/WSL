#!/usr/bin/env python3
# Copyright (C) Microsoft Corporation. All rights reserved.

"""ai_triage.py - AI-powered issue triage for microsoft/WSL (v1, dry-run).

Reads a GitHub issue, asks an LLM (via the gh-models extension) to classify it,
and upserts a single collapsible maintainer-facing comment with the analysis.

This is **dry-run only**: no labels are applied, no issue state is changed.
The agent is purely additive to the existing rule-based wti pipeline.

Design notes (see triage/ai/README.md and the project plan for full rationale):

* The LLM is treated as untrusted text generator. Its output is JSON-validated,
  then every field is intersected with a deterministic allowlist or with
  retrieval results we computed ourselves. Issue numbers the model returns are
  rejected unless they appear in the candidate list we passed in.
* Idempotency uses an input-sha hash embedded in the marker comment. If the
  issue is unchanged since the last run, we skip. After the model call we
  re-fetch and re-hash to detect stale runs (slow run vs newer edit) so the
  newer run wins.
* Failures (network, model, JSON, validation) are silent — we exit 0 with no
  comment, but log to stderr so the workflow run shows the cause.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import html
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

REPO = os.environ.get("AI_TRIAGE_REPO", "microsoft/WSL")
PROMPT_VERSION = "v1"
MARKER_PREFIX = "<!-- ai-triage:v1"

# Allowlists. Keep in sync with triage/ai/prompt.md.
COMPONENT_LABELS_ALLOWLIST: frozenset[str] = frozenset(
    {
        "network",
        "file system",
        "console",
        "interop",
        "GPU",
        "kernel",
        "systemd",
        "msix",
        "install",
        "distro-mgmt",
        "ARM",
        "wsl1",
        "wsl2",
        "Store WSL",
        "launcher",
        "/proc/",
        "kconfig",
        "hypervisor-platform",
        "i18n",
        "localization",
        "init-crash",
        "failure-to-launch",
        "ntbugcheck",
    }
)

ISSUE_TYPE_ALLOWLIST: frozenset[str] = frozenset(
    {"bug", "feature", "question", "discussion", "documentation", "enhancement", "unknown"}
)

MISSING_FIELD_ALLOWLIST: frozenset[str] = frozenset(
    {
        "Windows Version",
        "WSL Version",
        "WSL 1 vs WSL 2",
        "Repro Steps",
        "Expected Behavior",
        "Actual Behavior",
    }
)

# Skip rules.
SKIP_AUTHOR_ASSOCIATIONS: frozenset[str] = frozenset({"OWNER", "MEMBER", "COLLABORATOR"})
MIN_BODY_CHARS = 50

# Prompt size budget (characters, not tokens; conservative).
MAX_BODY_CHARS = 8000
BODY_TRUNCATION_NOTE = "\n\n[... body truncated by ai_triage.py ...]"

# Duplicate retrieval.
MAX_CANDIDATES = 15

# Default model. Configurable via --model or AI_TRIAGE_MODEL env var.
DEFAULT_MODEL = "openai/gpt-4o-mini"

PROMPT_PATH = Path(__file__).with_name("prompt.md")


# ---------------------------------------------------------------------------
# gh subprocess helpers
# ---------------------------------------------------------------------------


class GhError(RuntimeError):
    """Raised when a `gh` invocation fails. Always treated as silent abort."""


def _run(argv: list[str], *, input_text: str | None = None, check: bool = True) -> str:
    """Run a subprocess and return stdout. Raises GhError on non-zero exit."""
    try:
        result = subprocess.run(
            argv,
            input=input_text,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except FileNotFoundError as exc:
        raise GhError(f"binary not found: {argv[0]}") from exc

    if check and result.returncode != 0:
        raise GhError(
            f"command failed (exit {result.returncode}): {' '.join(argv)}\n"
            f"stderr: {result.stderr.strip()}"
        )
    return result.stdout


def gh_api(path: str, *, method: str = "GET", fields: dict[str, Any] | None = None) -> Any:
    argv = ["gh", "api", "--method", method, path]
    if fields:
        for key, value in fields.items():
            argv += ["-f", f"{key}={value}"]
    return json.loads(_run(argv))


def gh_api_raw_body(path: str, *, method: str, body: str) -> None:
    """POST/PATCH a raw JSON body via gh api --input -."""
    _run(
        ["gh", "api", "--method", method, "--input", "-", path],
        input_text=body,
    )


# ---------------------------------------------------------------------------
# Issue / candidate retrieval
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Issue:
    number: int
    title: str
    body: str
    state: str
    locked: bool
    author_login: str
    author_type: str  # "User" or "Bot"
    author_association: str  # OWNER / MEMBER / COLLABORATOR / CONTRIBUTOR / NONE / etc.


@dataclasses.dataclass(frozen=True)
class Candidate:
    number: int
    title: str
    state: str
    labels: tuple[str, ...]


def fetch_issue(number: int) -> Issue:
    data = gh_api(f"repos/{REPO}/issues/{number}")
    user = data.get("user") or {}
    return Issue(
        number=int(data["number"]),
        title=data.get("title") or "",
        body=data.get("body") or "",
        state=data.get("state") or "",
        locked=bool(data.get("locked")),
        author_login=user.get("login") or "",
        author_type=user.get("type") or "",
        author_association=data.get("author_association") or "NONE",
    )


_KEYWORD_PATTERN = re.compile(r"[A-Za-z][A-Za-z0-9_+#-]{2,}")
_STOPWORDS = frozenset(
    {
        "the",
        "and",
        "for",
        "with",
        "from",
        "this",
        "that",
        "when",
        "what",
        "have",
        "has",
        "not",
        "but",
        "are",
        "was",
        "were",
        "all",
        "any",
        "you",
        "your",
        "wsl",
        "windows",
        "linux",
        "issue",
        "bug",
        "error",
        "fail",
        "fails",
        "failed",
    }
)


def derive_search_query(title: str) -> str:
    """Pull a few content keywords out of the title for `gh search issues`."""
    tokens = []
    seen: set[str] = set()
    for match in _KEYWORD_PATTERN.findall(title):
        word = match.lower()
        if word in _STOPWORDS or word in seen:
            continue
        seen.add(word)
        tokens.append(match)
        if len(tokens) >= 5:
            break
    return " ".join(tokens)


def fetch_candidates(issue: Issue) -> list[Candidate]:
    """Top issues (open or closed) matching the title's content keywords.

    The caller's own issue is excluded. Maintainer-only labels are not stripped;
    the model uses them for context. We pass a maximum of MAX_CANDIDATES.
    """
    query = derive_search_query(issue.title)
    if not query:
        return []
    argv = [
        "gh",
        "search",
        "issues",
        "--repo",
        REPO,
        "--limit",
        str(MAX_CANDIDATES + 1),  # +1 because we may filter out the caller
        "--json",
        "number,title,state,labels",
        "--",
        query,
    ]
    try:
        out = _run(argv)
    except GhError as exc:
        print(f"warning: candidate search failed: {exc}", file=sys.stderr)
        return []
    raw = json.loads(out or "[]")
    result: list[Candidate] = []
    for entry in raw:
        number = int(entry.get("number", 0))
        if number == issue.number or number <= 0:
            continue
        labels = tuple(label.get("name", "") for label in (entry.get("labels") or []))
        result.append(
            Candidate(
                number=number,
                title=entry.get("title") or "",
                state=entry.get("state") or "",
                labels=labels,
            )
        )
        if len(result) >= MAX_CANDIDATES:
            break
    return result


def fetch_live_label_names() -> frozenset[str]:
    """Live label set, used for a final pass before suggesting labels."""
    try:
        out = _run(
            ["gh", "label", "list", "--repo", REPO, "--limit", "200", "--json", "name"]
        )
    except GhError as exc:
        print(f"warning: label list fetch failed: {exc}", file=sys.stderr)
        return frozenset()
    return frozenset(entry.get("name", "") for entry in json.loads(out or "[]"))


# ---------------------------------------------------------------------------
# Prompt rendering and hashing
# ---------------------------------------------------------------------------


def truncate(text: str, max_chars: int) -> str:
    if len(text) <= max_chars:
        return text
    return text[: max_chars - len(BODY_TRUNCATION_NOTE)] + BODY_TRUNCATION_NOTE


def render_prompt(template: str, issue: Issue, candidates: list[Candidate]) -> str:
    candidates_payload = [
        {
            "number": c.number,
            "title": c.title,
            "state": c.state,
            "labels": list(c.labels),
        }
        for c in candidates
    ]
    body = truncate(issue.body, MAX_BODY_CHARS)
    substitutions = {
        "{{ISSUE_NUMBER}}": str(issue.number),
        "{{ISSUE_TITLE}}": issue.title,
        "{{ISSUE_BODY}}": body,
        "{{CANDIDATES_JSON}}": json.dumps(candidates_payload, indent=2),
    }
    # Single-pass substitution. Sequential .replace() would let a later
    # replacement (e.g. {{CANDIDATES_JSON}}) rewrite content already
    # interpolated from an earlier untrusted field (issue title/body), giving
    # the issue author a way to alter the prompt. re.sub with a placeholder->
    # value map only touches placeholders present in the original template.
    pattern = re.compile("|".join(re.escape(k) for k in substitutions))
    return pattern.sub(lambda m: substitutions[m.group(0)], template)


def sha(*parts: str) -> str:
    h = hashlib.sha256()
    for part in parts:
        h.update(part.encode("utf-8", errors="replace"))
        h.update(b"\x00")
    return h.hexdigest()[:16]


def input_hash(issue: Issue) -> str:
    return sha(issue.title, issue.body, PROMPT_VERSION)


def prompt_hash(template: str) -> str:
    return sha(template, PROMPT_VERSION)


# ---------------------------------------------------------------------------
# Model call
# ---------------------------------------------------------------------------


def call_model(prompt: str, model: str) -> str:
    """Invoke `gh models run <model>` with the prompt on stdin. Returns stdout."""
    return _run(["gh", "models", "run", model], input_text=prompt)


# ---------------------------------------------------------------------------
# Output validation
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class TriageResult:
    issue_type: str
    component_labels: list[str]
    missing_fields: list[str]
    duplicate_candidate_numbers: list[int]
    maintainer_summary: str


def extract_json_object(text: str) -> dict[str, Any]:
    """Find the first balanced JSON object in text. Tolerates fenced output.

    Uses a brace-depth scanner instead of a single regex so multi-object output
    or trailing junk after the first object doesn't get merged into one
    invalid blob.
    """
    stripped = text.strip()
    if stripped.startswith("```"):
        stripped = re.sub(r"^```[a-zA-Z]*\n", "", stripped)
        stripped = re.sub(r"\n```\s*$", "", stripped)

    start = stripped.find("{")
    if start < 0:
        raise ValueError("no JSON object found in model output")

    depth = 0
    in_string = False
    escape = False
    for index in range(start, len(stripped)):
        char = stripped[index]
        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return json.loads(stripped[start : index + 1])

    raise ValueError("unbalanced JSON object in model output")


def _str_list(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [v for v in value if isinstance(v, str)]


def _int_list(value: Any) -> list[int]:
    if not isinstance(value, list):
        return []
    out: list[int] = []
    for v in value:
        if isinstance(v, bool):
            continue
        if isinstance(v, int):
            out.append(v)
        elif isinstance(v, str) and v.isdigit():
            out.append(int(v))
    return out


def validate_and_clamp(
    raw: dict[str, Any],
    *,
    candidate_numbers: set[int],
    live_labels: frozenset[str],
) -> TriageResult:
    issue_type = raw.get("issue_type") if isinstance(raw.get("issue_type"), str) else "unknown"
    if issue_type not in ISSUE_TYPE_ALLOWLIST:
        issue_type = "unknown"

    components = [
        label
        for label in _str_list(raw.get("component_labels"))
        if label in COMPONENT_LABELS_ALLOWLIST and (not live_labels or label in live_labels)
    ]
    # Deduplicate while preserving order.
    components = list(dict.fromkeys(components))

    missing = [f for f in _str_list(raw.get("missing_fields")) if f in MISSING_FIELD_ALLOWLIST]
    missing = list(dict.fromkeys(missing))

    duplicates = [n for n in _int_list(raw.get("duplicate_candidate_numbers")) if n in candidate_numbers]
    duplicates = list(dict.fromkeys(duplicates))[:5]

    summary_raw = raw.get("maintainer_summary")
    summary = summary_raw if isinstance(summary_raw, str) else ""
    summary = sanitize_summary(summary)

    return TriageResult(
        issue_type=issue_type,
        component_labels=components,
        missing_fields=missing,
        duplicate_candidate_numbers=duplicates,
        maintainer_summary=summary,
    )


_HTML_TAG_RE = re.compile(r"<[^>]+>")
_MARKDOWN_LINK_RE = re.compile(r"\[([^\]]+)\]\([^)]+\)")
_RAW_URL_RE = re.compile(r"https?://\S+")
_MENTION_RE = re.compile(r"(?<![\w])@(?=[A-Za-z0-9_-])")
_BACKTICK_RE = re.compile(r"`+")


def sanitize_summary(text: str) -> str:
    """Strip markdown links/URLs/mentions/backticks/HTML from model-authored prose.

    Defense-in-depth: even though render_comment() also html-escapes the result,
    we strip raw HTML tags here so the function lives up to its name and is safe
    in isolation if it's ever used outside the rendering path.
    """
    if not text:
        return ""
    cleaned = _HTML_TAG_RE.sub("", text)
    cleaned = _MARKDOWN_LINK_RE.sub(r"\1", cleaned)
    cleaned = _RAW_URL_RE.sub("[link removed]", cleaned)
    # Defang @mentions by inserting a zero-width space after the @.
    cleaned = _MENTION_RE.sub("@\u200b", cleaned)
    cleaned = _BACKTICK_RE.sub("'", cleaned)
    cleaned = cleaned.strip()
    if len(cleaned) > 400:
        cleaned = cleaned[:397].rstrip() + "..."
    return cleaned


# ---------------------------------------------------------------------------
# Comment rendering and upsert
# ---------------------------------------------------------------------------


def render_marker(input_sha: str, prompt_sha: str) -> str:
    return f"<!-- ai-triage:v1 input-sha={input_sha} prompt-sha={prompt_sha} -->"


def render_comment(
    result: TriageResult, candidates: list[Candidate], marker: str, model: str
) -> str:
    cand_by_num = {c.number: c for c in candidates}

    def fmt_labels(labels: Iterable[str]) -> str:
        items = list(labels)
        if not items:
            return "_none_"
        return ", ".join(f"`{html.escape(label)}`" for label in items)

    summary = html.escape(result.maintainer_summary or "_(no summary produced)_")

    lines: list[str] = [
        marker,
        "<details><summary>🤖 AI triage summary (suggestions, dry-run — not auto-applied)</summary>",
        "",
        f"**Summary:** {summary}",
        "",
        f"**Suggested type:** `{html.escape(result.issue_type)}`",
        "",
        f"**Suggested component labels:** {fmt_labels(result.component_labels)}",
        "",
    ]

    if result.missing_fields:
        missing = ", ".join(f"`{html.escape(f)}`" for f in result.missing_fields)
        lines += [f"**Missing template fields:** {missing}", ""]

    if result.duplicate_candidate_numbers:
        lines.append("**Possible duplicates:**")
        for number in result.duplicate_candidate_numbers:
            cand = cand_by_num.get(number)
            title = html.escape(cand.title) if cand else ""
            lines.append(f"- #{number} — {title}")
        lines.append("")

    timestamp = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    lines += [
        f"<sub>Generated by ai_triage {PROMPT_VERSION} · model: <code>{html.escape(model)}</code> · {timestamp}</sub>",
        "",
        "</details>",
    ]
    return "\n".join(lines)


_COMMENT_PAGE_LIMIT = 10  # cap pagination at 1000 comments; well above any real issue


def find_existing_marker_comment(issue_number: int) -> dict[str, Any] | None:
    """Return our most recent marker comment, or None.

    Walks pages newest-first (sort=created&direction=desc) and stops at the
    first marker hit. If no marker appears in the first 100 comments and the
    issue has more than 100, we keep paginating until either we find one, the
    page comes back short (last page), or we hit the safety cap.
    """
    for page in range(1, _COMMENT_PAGE_LIMIT + 1):
        comments = gh_api(
            f"repos/{REPO}/issues/{issue_number}/comments"
            f"?per_page=100&sort=created&direction=desc&page={page}"
        )
        if not isinstance(comments, list) or not comments:
            return None
        for comment in comments:
            body = comment.get("body") if isinstance(comment, dict) else None
            if isinstance(body, str) and MARKER_PREFIX in body:
                return comment
        if len(comments) < 100:
            return None
    return None


_MARKER_FIELDS_RE = re.compile(r"<!--\s*ai-triage:v1\s+input-sha=([0-9a-f]+)\s+prompt-sha=([0-9a-f]+)\s*-->")


def parse_marker(body: str) -> tuple[str, str] | None:
    match = _MARKER_FIELDS_RE.search(body or "")
    if not match:
        return None
    return match.group(1), match.group(2)


def upsert_comment(issue_number: int, comment_body: str, existing: dict[str, Any] | None) -> None:
    payload = json.dumps({"body": comment_body})
    if existing and isinstance(existing.get("id"), int):
        gh_api_raw_body(
            f"repos/{REPO}/issues/comments/{existing['id']}",
            method="PATCH",
            body=payload,
        )
    else:
        gh_api_raw_body(
            f"repos/{REPO}/issues/{issue_number}/comments",
            method="POST",
            body=payload,
        )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--issue", type=int, required=True, help="issue number to triage")
    parser.add_argument(
        "--model",
        default=os.environ.get("AI_TRIAGE_MODEL", DEFAULT_MODEL),
        help=f"GitHub Models identifier (default: {DEFAULT_MODEL})",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the rendered comment to stdout instead of posting",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="ignore the input-sha skip check (still respects skip rules)",
    )
    return parser.parse_args(argv)


def should_skip(issue: Issue) -> str | None:
    if issue.state != "open":
        return f"issue #{issue.number} is not open (state={issue.state})"
    if issue.locked:
        return f"issue #{issue.number} is locked"
    if issue.author_type == "Bot" or issue.author_login.endswith("[bot]"):
        return f"author {issue.author_login!r} is a bot"
    if issue.author_association in SKIP_AUTHOR_ASSOCIATIONS:
        return f"author association {issue.author_association} is maintainer-level"
    if len(issue.body.strip()) < MIN_BODY_CHARS:
        return f"body is shorter than {MIN_BODY_CHARS} characters"
    return None


def main(argv: list[str]) -> int:
    try:
        return _main_inner(argv)
    except SystemExit:
        raise
    except Exception as exc:
        # Anything reaching here escaped the inline GhError handlers in
        # _main_inner and is therefore unexpected (programming bug, permission
        # misconfig such as the comment-upsert 403, etc.). Surface it loudly
        # so the workflow run fails and maintainers see it. Expected silent
        # failures (model errors, JSON parse errors, transient gh API errors
        # on read paths) are caught and converted to exit-0 inline.
        import traceback

        print(f"ERROR: unexpected {type(exc).__name__}: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1


def _main_inner(argv: list[str]) -> int:
    args = parse_args(argv)

    template = PROMPT_PATH.read_text(encoding="utf-8")
    p_sha = prompt_hash(template)

    try:
        issue = fetch_issue(args.issue)
    except GhError as exc:
        print(f"abort: failed to fetch issue: {exc}", file=sys.stderr)
        return 0  # silent

    skip_reason = should_skip(issue)
    if skip_reason:
        print(f"skip: {skip_reason}", file=sys.stderr)
        return 0

    in_sha = input_hash(issue)
    existing = None
    if not args.dry_run:
        try:
            existing = find_existing_marker_comment(issue.number)
        except GhError as exc:
            print(f"abort: failed to fetch existing comments: {exc}", file=sys.stderr)
            return 0
    if existing and not args.force:
        marker_fields = parse_marker(existing.get("body") or "")
        if marker_fields == (in_sha, p_sha):
            print(f"skip: comment already up-to-date (input-sha={in_sha})", file=sys.stderr)
            return 0

    candidates = fetch_candidates(issue)
    candidate_numbers = {c.number for c in candidates}
    live_labels = fetch_live_label_names()

    prompt = render_prompt(template, issue, candidates)

    try:
        raw_response = call_model(prompt, args.model)
    except GhError as exc:
        print(f"abort: model call failed: {exc}", file=sys.stderr)
        return 0  # silent

    try:
        parsed = extract_json_object(raw_response)
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"abort: model output not valid JSON: {exc}", file=sys.stderr)
        print(f"raw response: {raw_response!r}", file=sys.stderr)
        return 0  # silent

    result = validate_and_clamp(parsed, candidate_numbers=candidate_numbers, live_labels=live_labels)

    # Stale-run protection: re-fetch and recompute hash; abort if changed.
    try:
        latest = fetch_issue(args.issue)
    except GhError as exc:
        print(f"abort: failed to re-fetch issue for staleness check: {exc}", file=sys.stderr)
        return 0
    if input_hash(latest) != in_sha:
        print(
            f"abort: issue #{args.issue} changed during model call; deferring to next run",
            file=sys.stderr,
        )
        return 0

    marker = render_marker(in_sha, p_sha)
    comment_body = render_comment(result, candidates, marker, args.model)

    if args.dry_run:
        print(comment_body)
        return 0

    # Intentionally NOT wrapped: an upsert failure (e.g. permission 403, 5xx)
    # means we built a valid comment but couldn't post it. That is a maintainer-
    # actionable misconfiguration, not transient model noise, so we let it
    # propagate to main() and fail the workflow run loudly.
    upsert_comment(args.issue, comment_body, existing)

    print(f"posted ai-triage comment on issue #{args.issue}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
