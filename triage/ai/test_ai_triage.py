# Copyright (C) Microsoft Corporation. All rights reserved.

"""Unit tests for triage/ai/ai_triage.py.

Pure-function only — no network, no subprocess, no model calls. These tests
gate the security-critical validation/sanitization logic and document the
expected behavior for future maintainers.

Run:    python -m pytest triage/ai
"""

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ai_triage as a  # noqa: E402


# ---------------------------------------------------------------------------
# sanitize_summary
# ---------------------------------------------------------------------------


class TestSanitizeSummary:
    def test_empty_input(self):
        assert a.sanitize_summary("") == ""

    def test_strips_markdown_link(self):
        assert a.sanitize_summary("see [docs](https://example.com)") == "see docs"

    def test_strips_raw_url(self):
        result = a.sanitize_summary("go to https://example.com now")
        assert "https://" not in result
        assert "[link removed]" in result

    def test_defangs_at_mention(self):
        result = a.sanitize_summary("hi @octocat thanks")
        assert "@octocat" not in result
        assert "@\u200boctocat" in result

    def test_does_not_defang_email_local_part(self):
        # The negative lookbehind (?<![\w]) means an @ preceded by a word
        # char (the local part of an email) is NOT treated as a mention.
        result = a.sanitize_summary("contact support@example.com later")
        assert "support@example.com" in result
        # And specifically, no zero-width-space was inserted after the @.
        assert "@\u200b" not in result

    def test_strips_backticks(self):
        assert a.sanitize_summary("a `code` block") == "a 'code' block"

    def test_clamps_length_to_400(self):
        long = a.sanitize_summary("a" * 500)
        assert len(long) == 400
        assert long.endswith("...")

    def test_does_not_clamp_short_strings(self):
        text = "short summary"
        assert a.sanitize_summary(text) == text

    def test_strips_surrounding_whitespace(self):
        assert a.sanitize_summary("  hello  ") == "hello"

    def test_strips_html_tags(self):
        # Defense-in-depth: render_comment also html-escapes, but the function
        # is named "sanitize" and must produce safe output in isolation.
        assert a.sanitize_summary("<script>alert(1)</script>hello") == "alert(1)hello"

    def test_strips_html_tag_attributes(self):
        result = a.sanitize_summary('click <a href="https://evil">here</a>')
        assert "<" not in result
        assert ">" not in result
        assert "href" not in result
        assert "here" in result

    def test_strips_html_comment(self):
        # HTML comments would otherwise let the model inject a fake marker.
        assert "<!--" not in a.sanitize_summary("hi <!-- ai-triage:v1 fake --> bye")


# ---------------------------------------------------------------------------
# extract_json_object
# ---------------------------------------------------------------------------


class TestExtractJsonObject:
    def test_bare_object(self):
        assert a.extract_json_object('{"a": 1}') == {"a": 1}

    def test_fenced_with_language(self):
        text = '```json\n{"a": 1, "b": [2, 3]}\n```'
        assert a.extract_json_object(text) == {"a": 1, "b": [2, 3]}

    def test_fenced_without_language(self):
        text = "```\n{\"a\": 1}\n```"
        assert a.extract_json_object(text) == {"a": 1}

    def test_garbage_prefix(self):
        assert a.extract_json_object('Sure, here is the JSON:\n{"a": 1}') == {"a": 1}

    def test_garbage_suffix(self):
        # Reviewer-flagged regression: the old greedy regex matched
        # everything between the first { and the LAST }, merging two objects.
        assert a.extract_json_object('{"a": 1} some trailing text') == {"a": 1}

    def test_multiple_objects_returns_first(self):
        # Same reviewer-flagged case.
        assert a.extract_json_object('{"a": 1} {"b": 2}') == {"a": 1}

    def test_nested_object(self):
        assert a.extract_json_object('{"a": {"b": {"c": 1}}}') == {"a": {"b": {"c": 1}}}

    def test_string_containing_braces(self):
        # The brace-depth scanner must not be confused by braces inside strings.
        assert a.extract_json_object('{"a": "}{"}') == {"a": "}{"}

    def test_string_containing_escaped_quote(self):
        assert a.extract_json_object('{"a": "he said \\"hi\\""}') == {"a": 'he said "hi"'}

    def test_no_braces_raises(self):
        with pytest.raises(ValueError):
            a.extract_json_object("no json here")

    def test_unbalanced_raises(self):
        with pytest.raises(ValueError):
            a.extract_json_object('{"a": 1')


# ---------------------------------------------------------------------------
# validate_and_clamp
# ---------------------------------------------------------------------------


class TestValidateAndClamp:
    def _base(self, **overrides):
        return {
            "issue_type": "bug",
            "component_labels": [],
            "missing_fields": [],
            "duplicate_candidate_numbers": [],
            "maintainer_summary": "x",
            **overrides,
        }

    def test_known_type_passes(self):
        result = a.validate_and_clamp(self._base(), candidate_numbers=set(), live_labels=frozenset())
        assert result.issue_type == "bug"

    def test_unknown_type_collapses(self):
        result = a.validate_and_clamp(
            self._base(issue_type="UNKNOWN-TYPE"),
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert result.issue_type == "unknown"

    def test_non_string_type_collapses(self):
        result = a.validate_and_clamp(
            self._base(issue_type=42),
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert result.issue_type == "unknown"

    def test_component_labels_intersected_with_static_allowlist(self):
        result = a.validate_and_clamp(
            self._base(component_labels=["network", "fake-label", "msix"]),
            candidate_numbers=set(),
            live_labels=frozenset(),  # disabled
        )
        assert result.component_labels == ["network", "msix"]

    def test_component_labels_intersected_with_live_labels(self):
        result = a.validate_and_clamp(
            self._base(component_labels=["network", "msix"]),
            candidate_numbers=set(),
            live_labels=frozenset({"network"}),  # msix not in live set
        )
        assert result.component_labels == ["network"]

    def test_component_labels_dedup_preserves_order(self):
        result = a.validate_and_clamp(
            self._base(component_labels=["msix", "network", "msix"]),
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert result.component_labels == ["msix", "network"]

    def test_missing_fields_intersected_with_allowlist(self):
        result = a.validate_and_clamp(
            self._base(missing_fields=["Windows Version", "Bogus", "Repro Steps"]),
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert result.missing_fields == ["Windows Version", "Repro Steps"]

    def test_duplicate_numbers_intersected_with_candidates(self):
        result = a.validate_and_clamp(
            self._base(duplicate_candidate_numbers=[1, 2, 9999]),
            candidate_numbers={1, 2},
            live_labels=frozenset(),
        )
        assert result.duplicate_candidate_numbers == [1, 2]

    def test_duplicate_numbers_capped_at_five(self):
        result = a.validate_and_clamp(
            self._base(duplicate_candidate_numbers=list(range(1, 11))),
            candidate_numbers=set(range(1, 11)),
            live_labels=frozenset(),
        )
        assert result.duplicate_candidate_numbers == [1, 2, 3, 4, 5]

    def test_duplicate_numbers_string_digits_accepted(self):
        result = a.validate_and_clamp(
            self._base(duplicate_candidate_numbers=["1", "2", "abc"]),
            candidate_numbers={1, 2},
            live_labels=frozenset(),
        )
        assert result.duplicate_candidate_numbers == [1, 2]

    def test_duplicate_numbers_booleans_rejected(self):
        # Python: bool is subclass of int, so True == 1. Must not slip through.
        result = a.validate_and_clamp(
            self._base(duplicate_candidate_numbers=[True, 2]),
            candidate_numbers={1, 2},
            live_labels=frozenset(),
        )
        assert result.duplicate_candidate_numbers == [2]

    def test_summary_sanitization_applied(self):
        result = a.validate_and_clamp(
            self._base(maintainer_summary="hi @user see https://x.com"),
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert "@\u200buser" in result.maintainer_summary
        assert "https://" not in result.maintainer_summary

    def test_non_list_fields_become_empty(self):
        result = a.validate_and_clamp(
            self._base(component_labels="network", missing_fields=None, duplicate_candidate_numbers="1,2"),
            candidate_numbers={1, 2},
            live_labels=frozenset(),
        )
        assert result.component_labels == []
        assert result.missing_fields == []
        assert result.duplicate_candidate_numbers == []

    def test_missing_keys_use_defaults(self):
        result = a.validate_and_clamp(
            {"issue_type": "bug"},
            candidate_numbers=set(),
            live_labels=frozenset(),
        )
        assert result.component_labels == []
        assert result.missing_fields == []
        assert result.duplicate_candidate_numbers == []
        assert result.maintainer_summary == ""

    def test_static_allowlist_matches_prompt_template(self):
        # Drift guard: every label suggested in the prompt must be in the
        # Python allowlist, so a model that quotes the prompt verbatim
        # won't have its labels silently dropped.
        prompt_text = a.PROMPT_PATH.read_text(encoding="utf-8")
        for label in a.COMPONENT_LABELS_ALLOWLIST:
            assert f"`{label}`" in prompt_text, f"label {label!r} missing from prompt"


# ---------------------------------------------------------------------------
# derive_search_query
# ---------------------------------------------------------------------------


class TestDeriveSearchQuery:
    def test_extracts_content_keywords(self):
        q = a.derive_search_query("WSL fails to mount drvfs share with permission denied error")
        tokens = q.split()
        assert "drvfs" in [t.lower() for t in tokens]

    def test_strips_stopwords(self):
        q = a.derive_search_query("the and for with from")
        assert q == ""

    def test_strips_wsl_stopword(self):
        # 'wsl' alone is a stopword (every issue is about WSL).
        q = a.derive_search_query("wsl wsl wsl drvfs")
        assert "wsl" not in q.lower().split()
        assert "drvfs" in q.lower()

    def test_dedups_keywords(self):
        q = a.derive_search_query("drvfs drvfs DRVFS mount")
        # Dedup is case-insensitive but original casing of first occurrence
        # wins. Either way, only one drvfs should appear.
        tokens = [t.lower() for t in q.split()]
        assert tokens.count("drvfs") == 1

    def test_caps_at_five_tokens(self):
        q = a.derive_search_query("alpha beta gamma delta epsilon zeta eta theta")
        assert len(q.split()) == 5

    def test_filters_short_tokens(self):
        # Token regex requires 3+ alphanumerics after first letter.
        q = a.derive_search_query("a bb ccc dddd")
        for token in q.split():
            assert len(token) >= 3

    def test_empty_title(self):
        assert a.derive_search_query("") == ""


# ---------------------------------------------------------------------------
# Hashing & marker round-trip
# ---------------------------------------------------------------------------


class TestHashing:
    def _issue(self, title: str = "t", body: str = "b") -> SimpleNamespace:
        return SimpleNamespace(title=title, body=body)

    def test_input_hash_stable(self):
        assert a.input_hash(self._issue()) == a.input_hash(self._issue())

    def test_input_hash_changes_with_body(self):
        assert a.input_hash(self._issue(body="x")) != a.input_hash(self._issue(body="y"))

    def test_input_hash_changes_with_title(self):
        assert a.input_hash(self._issue(title="x")) != a.input_hash(self._issue(title="y"))

    def test_input_hash_field_separation(self):
        # title="ab", body="" must not collide with title="a", body="b".
        h1 = a.input_hash(self._issue(title="ab", body=""))
        h2 = a.input_hash(self._issue(title="a", body="b"))
        assert h1 != h2

    def test_prompt_hash_changes_with_template(self):
        assert a.prompt_hash("v1: hello") != a.prompt_hash("v1: world")


class TestMarker:
    def test_round_trip(self):
        marker = a.render_marker("aaaa1111", "bbbb2222")
        assert a.parse_marker(marker) == ("aaaa1111", "bbbb2222")

    def test_no_marker_returns_none(self):
        assert a.parse_marker("just a normal comment body") is None

    def test_marker_inside_larger_body(self):
        body = "intro\n<!-- ai-triage:v1 input-sha=abc12345 prompt-sha=def67890 -->\nbody"
        assert a.parse_marker(body) == ("abc12345", "def67890")

    def test_v1_marker_prefix_constant(self):
        # If MARKER_PREFIX changes, render_marker output must still start with it.
        marker = a.render_marker("a" * 16, "b" * 16)
        assert marker.startswith(a.MARKER_PREFIX)


# ---------------------------------------------------------------------------
# should_skip
# ---------------------------------------------------------------------------


class TestShouldSkip:
    def _issue(self, **overrides) -> SimpleNamespace:
        defaults = dict(
            number=1,
            state="open",
            locked=False,
            author_login="alice",
            author_type="User",
            author_association="NONE",
            body="x" * 200,
            title="hi",
        )
        defaults.update(overrides)
        return SimpleNamespace(**defaults)

    def test_open_user_issue_is_not_skipped(self):
        assert a.should_skip(self._issue()) is None

    def test_closed_issue_is_skipped(self):
        assert a.should_skip(self._issue(state="closed")) is not None

    def test_locked_issue_is_skipped(self):
        assert a.should_skip(self._issue(locked=True)) is not None

    def test_bot_by_type_is_skipped(self):
        assert a.should_skip(self._issue(author_type="Bot")) is not None

    def test_bot_by_login_suffix_is_skipped(self):
        assert a.should_skip(self._issue(author_login="dependabot[bot]")) is not None

    @pytest.mark.parametrize("association", ["OWNER", "MEMBER", "COLLABORATOR"])
    def test_maintainer_association_is_skipped(self, association):
        assert a.should_skip(self._issue(author_association=association)) is not None

    @pytest.mark.parametrize("association", ["NONE", "CONTRIBUTOR", "FIRST_TIME_CONTRIBUTOR", "MANNEQUIN"])
    def test_non_maintainer_association_is_not_skipped(self, association):
        assert a.should_skip(self._issue(author_association=association)) is None

    def test_short_body_is_skipped(self):
        assert a.should_skip(self._issue(body="too short")) is not None

    def test_body_at_threshold_is_not_skipped(self):
        assert a.should_skip(self._issue(body="x" * a.MIN_BODY_CHARS)) is None

    def test_whitespace_only_body_is_skipped(self):
        # body.strip() < MIN_BODY_CHARS
        assert a.should_skip(self._issue(body=" " * 200)) is not None


# ---------------------------------------------------------------------------
# truncate
# ---------------------------------------------------------------------------


class TestTruncate:
    def test_short_text_unchanged(self):
        assert a.truncate("hello", 100) == "hello"

    def test_exact_length_unchanged(self):
        text = "x" * 100
        assert a.truncate(text, 100) == text

    def test_long_text_truncated_with_note(self):
        result = a.truncate("x" * 1000, 200)
        assert len(result) == 200
        assert result.endswith(a.BODY_TRUNCATION_NOTE)


# ---------------------------------------------------------------------------
# render_comment / render_marker
# ---------------------------------------------------------------------------


class TestRenderComment:
    def _result(self, **overrides) -> a.TriageResult:
        defaults = dict(
            issue_type="bug",
            component_labels=["network"],
            missing_fields=["Windows Version"],
            duplicate_candidate_numbers=[42],
            maintainer_summary="Networking fails after update.",
        )
        defaults.update(overrides)
        return a.TriageResult(**defaults)

    def _candidates(self, *numbers_titles) -> list[a.Candidate]:
        return [
            a.Candidate(number=n, title=t, state="open", labels=())
            for n, t in numbers_titles
        ]

    def test_marker_first_line(self):
        marker = a.render_marker("a" * 16, "b" * 16)
        text = a.render_comment(self._result(), self._candidates((42, "x")), marker, "m")
        assert text.startswith(marker + "\n")

    def test_html_escapes_candidate_title(self):
        text = a.render_comment(
            self._result(),
            self._candidates((42, "<script>alert(1)</script>")),
            a.render_marker("a", "b"),
            "m",
        )
        assert "<script>" not in text
        assert "&lt;script&gt;" in text

    def test_html_escapes_summary(self):
        text = a.render_comment(
            self._result(maintainer_summary="<b>bold</b>"),
            [],
            a.render_marker("a", "b"),
            "m",
        )
        assert "<b>bold</b>" not in text
        assert "&lt;b&gt;bold&lt;/b&gt;" in text

    def test_html_escapes_model_id(self):
        text = a.render_comment(
            self._result(), [], a.render_marker("a", "b"), "<bad>"
        )
        assert "<bad>" not in text
        assert "&lt;bad&gt;" in text

    def test_omits_missing_fields_section_when_empty(self):
        text = a.render_comment(
            self._result(missing_fields=[]),
            [],
            a.render_marker("a", "b"),
            "m",
        )
        assert "Missing template fields" not in text

    def test_omits_duplicates_section_when_empty(self):
        text = a.render_comment(
            self._result(duplicate_candidate_numbers=[]),
            [],
            a.render_marker("a", "b"),
            "m",
        )
        assert "Possible duplicates" not in text

    def test_includes_dryrun_disclaimer(self):
        text = a.render_comment(self._result(), [], a.render_marker("a", "b"), "m")
        assert "dry-run" in text.lower()


# ---------------------------------------------------------------------------
# Allowlist / prompt drift guard
# ---------------------------------------------------------------------------


class TestAllowlistConsistency:
    def test_issue_types_in_prompt(self):
        prompt_text = a.PROMPT_PATH.read_text(encoding="utf-8")
        for issue_type in a.ISSUE_TYPE_ALLOWLIST:
            assert f"`{issue_type}`" in prompt_text, f"type {issue_type!r} missing from prompt"

    def test_missing_fields_in_prompt(self):
        prompt_text = a.PROMPT_PATH.read_text(encoding="utf-8")
        for field in a.MISSING_FIELD_ALLOWLIST:
            assert f"`{field}`" in prompt_text, f"missing-field {field!r} missing from prompt"

    def test_marker_prefix_used_consistently(self):
        # The MARKER_PREFIX constant must appear at the start of every render.
        sample = a.render_marker("0" * 16, "1" * 16)
        assert sample.startswith(a.MARKER_PREFIX)
        # And parse_marker must recognize what render_marker produces.
        assert a.parse_marker(sample) == ("0" * 16, "1" * 16)
