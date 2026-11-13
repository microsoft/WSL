# Copyright (C) Microsoft Corporation. All rights reserved.

import contextlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import backport_utils as utils
import gen_cherrypick_prs as generate
import relabel_backported as relabel


class BackportTests(unittest.TestCase):
    def setUp(self):
        self.original_directory = Path.cwd()
        self.template = Path(__file__).resolve().parents[2] / ".github" / "pull_request_template.md"
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(os.chdir, self.original_directory)
        os.chdir(self.directory.name)
        self.real_run = utils.run
        self.git("init", "--initial-branch=master")
        self.git("config", "user.name", "Backport Test")
        self.git("config", "user.email", "backport-test@example.invalid")
        Path(".github").mkdir()
        shutil.copyfile(self.template, Path(".github") / "pull_request_template.md")
        self.base = self.commit("base.txt", "baseline\n")
        self.first = self.commit("first.txt", "first\n")
        self.second = self.commit("second.txt", "second\n")
        self.git("branch", "release/3.0", self.base)
        self.git("update-ref", "refs/remotes/origin/release/3.0", self.base)
        self.prs = {
            1: self.pr(1, self.first, "2026-11-01T00:00:00Z"),
            2: self.pr(2, self.second, "2026-11-02T00:00:00Z"),
        }
        self.candidates = []
        self.calls = []
        self.labels = [{"name": "backported_3.0"}]
        self.real_fetch = False
        self.real_push = False
        self.real_remote_urls = False
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(utils, "run", side_effect=self.fake_run))
        self.stack.enter_context(patch.object(generate, "run", side_effect=self.fake_run))

    def git(self, *args):
        return self.real_run("git", *args).stdout.strip()

    def commit(self, filename, content):
        Path(filename).write_text(content, encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "--no-verify", "-m", filename)
        return self.git("rev-parse", "HEAD")

    def pr(self, number, sha, merged):
        return {
            "number": number, "title": f"Fix {number}", "url": f"https://github.com/microsoft/WSL/pull/{number}",
            "state": "MERGED", "mergedAt": merged, "mergeCommit": {"oid": sha}, "body": "Original body",
            "baseRefName": "master", "labels": [{"name": "backport_3.0"}],
        }

    def fake_run(self, *args, input=None, check=True):
        self.calls.append((args, input))
        value = ""
        if args[:3] == ("git", "remote", "get-url"):
            if self.real_remote_urls:
                return self.real_run(*args, input=input, check=check)
            value = "https://github.com/microsoft/WSL.git"
        elif args[:2] == ("git", "fetch"):
            if self.real_fetch:
                return self.real_run(*args, input=input, check=check)
        elif args[:2] == ("git", "push"):
            if self.real_push:
                return self.real_run(*args, input=input, check=check)
        elif args[:3] == ("gh", "repo", "view"):
            value = json.dumps({"defaultBranchRef": {"name": "master"}})
        elif args[:3] == ("gh", "pr", "view"):
            value = json.dumps(self.prs[int(args[3])])
        elif args[:3] == ("gh", "pr", "list"):
            base = args[args.index("--base") + 1]
            value = json.dumps(list(reversed(list(self.prs.values()))) if base == "master" else self.candidates)
        elif args[:2] == ("gh", "api"):
            value = json.dumps([self.labels])
        elif args[:3] == ("gh", "pr", "create"):
            value = "https://github.com/microsoft/WSL/pull/100"
        elif args[:3] in (("gh", "pr", "edit"), ("gh", "label", "create")):
            pass
        else:
            return self.real_run(*args, input=input, check=check)
        return subprocess.CompletedProcess(args, 0, value, "")

    def invoke(self, function, args):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            code = function(args)
        return code, stream.getvalue()

    def mutations(self):
        prefixes = {
            ("git", "switch"), ("git", "cherry-pick"), ("git", "push"),
            ("gh", "pr", "create"), ("gh", "pr", "edit"), ("gh", "label", "create"),
        }
        return [args for args, _ in self.calls if args[:2] in prefixes or args[:3] in prefixes]

    def test_preview_orders_picks_and_does_not_mutate(self):
        before = self.git("rev-parse", "HEAD")
        code, text = self.invoke(generate.main, ["3.0", "2", "#1", "2"])
        self.assertEqual(code, 0)
        self.assertLess(text.index("\n#1:"), text.index("\n#2:"))
        self.assertEqual(before, self.git("rev-parse", "HEAD"))
        self.assertEqual(self.mutations(), [])

    def test_apply_creates_independent_branches_and_template_prs(self):
        code, _ = self.invoke(generate.main, ["3.0", "2", "1", "--apply", "--draft"])
        self.assertEqual(code, 0)
        for number, sha in ((1, self.first), (2, self.second)):
            head = f"cherrypick/release/3.0/pr-{number}"
            self.assertEqual(self.git("rev-parse", f"{head}^"), self.base)
            self.assertIn(f"(cherry picked from commit {sha})", self.git("log", "-1", "--format=%B", head))
        created = [(args, body) for args, body in self.calls if args[:3] == ("gh", "pr", "create")]
        self.assertEqual(len(created), 2)
        template = self.template.read_text(encoding="utf-8")
        for args, body in created:
            self.assertIn("--draft", args)
            self.assertEqual(args[args.index("--base") + 1], "release/3.0")
            self.assertIn("Build and tests: not run by this script.", body)
            for line in template.splitlines():
                if line.startswith(("## ", "<!--", "- [ ]")):
                    self.assertIn(line, body)
        self.assertFalse(any("--force" in arg for args, _ in self.calls for arg in args))

    def test_dirty_tree_stops_before_git_changes(self):
        Path("untracked.txt").write_text("keep me", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "clean"):
            self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(self.mutations(), [])
        self.assertTrue(Path("untracked.txt").exists())

    def test_existing_branch_is_not_reset_or_pushed(self):
        head = "cherrypick/release/3.0/pr-1"
        self.git("branch", head, self.second)
        code, text = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertIn("existing local branch", text)
        self.assertEqual(self.git("rev-parse", head), self.second)
        self.assertFalse(any(args[:2] == ("git", "push") for args, _ in self.calls))

    def test_aborted_attempt_does_not_block_later_prs(self):
        self.git("switch", "release/3.0")
        conflicting = self.commit("first.txt", "competing release change\n")
        self.git("update-ref", "refs/remotes/origin/release/3.0", conflicting)
        self.git("switch", "master")
        self.assertEqual(self.invoke(generate.main, ["3.0", "1", "2", "--apply"])[0], 2)
        self.git("cherry-pick", "--abort")
        self.calls.clear()
        code, text = self.invoke(generate.main, ["3.0", "1", "2", "--apply"])
        self.assertEqual(code, 0)
        self.assertIn("Skipping #1: existing local branch", text)
        self.assertEqual(self.git("rev-parse", "cherrypick/release/3.0/pr-1"), conflicting)
        created = [args for args, _ in self.calls if args[:3] == ("gh", "pr", "create")]
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0][created[0].index("--head") + 1], "cherrypick/release/3.0/pr-2")

    def test_conflict_stops_before_push_and_preserves_resolution_state(self):
        self.git("switch", "release/3.0")
        conflicting = self.commit("first.txt", "competing release change\n")
        self.git("update-ref", "refs/remotes/origin/release/3.0", conflicting)
        self.git("switch", "master")
        code, text = self.invoke(generate.main, ["3.0", "1", "2", "--apply"])
        self.assertEqual(code, 2)
        self.assertIn("Stopped before push/PR creation", text)
        self.assertEqual(self.git("rev-parse", "CHERRY_PICK_HEAD"), self.first)
        self.assertFalse(any(args[:2] == ("git", "push") for args, _ in self.calls))
        self.assertFalse(any(args[:3] == ("gh", "pr", "create") for args, _ in self.calls))

    def test_active_backports_use_exact_provenance_not_similar_numbers(self):
        self.candidates = [
            {"number": 10, "state": "OPEN", "url": "existing",
             "body": "Backport of https://github.com/microsoft/WSL/pull/12 to release/3.0."},
            {"number": 11, "state": "CLOSED", "url": "abandoned", "body": "Clean cherry pick of PR #1"},
        ]
        self.assertEqual(utils.active_backports(self.prs[1], self.candidates, set()), [])
        self.candidates.append(
            {"number": 12, "state": "OPEN", "url": "open", "body": "Clean cherry pick of PR #1\n"}
        )
        code, text = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertIn("existing backport open", text)
        self.assertEqual(self.mutations(), [])

    def test_standard_revert_keeps_label_pending_and_allows_a_new_pick(self):
        self.git("switch", "release/3.0")
        self.git("cherry-pick", "-x", self.first)
        backport = self.git("rev-parse", "HEAD")
        self.git("revert", "--no-edit", backport)
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        self.candidates = [
            {"number": 10, "state": "MERGED", "url": "reverted",
             "body": "Clean cherry pick of PR #1", "mergeCommit": {"oid": backport}},
        ]
        self.assertEqual(self.invoke(relabel.main, ["3.0", "--apply"])[0], 0)
        self.assertEqual(self.mutations(), [])
        code, text = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertNotIn("Skipping #1", text)
        self.assertEqual(sum(args[:3] == ("gh", "pr", "create") for args, _ in self.calls), 1)

    def test_reverted_cherry_pick_in_merged_backport_allows_a_new_pick(self):
        self.git("switch", "--detach", self.base)
        self.git("cherry-pick", "-x", self.first)
        backport = self.git("rev-parse", "HEAD")
        self.git("switch", "release/3.0")
        self.git("merge", "--no-ff", "-m", "Merge backport", backport)
        merge = self.git("rev-parse", "HEAD")
        self.git("revert", "--no-edit", backport)
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        self.candidates = [
            {"number": 10, "state": "MERGED", "url": "reverted",
             "body": "Clean cherry pick of PR #1", "mergeCommit": {"oid": merge}},
        ]
        present = utils.backported_commits("HEAD")
        self.assertIn(merge, present)
        self.assertNotIn(self.first, present)
        self.assertEqual(self.invoke(relabel.main, ["3.0", "--apply"])[0], 0)
        self.assertEqual(self.mutations(), [])
        code, text = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertNotIn("Skipping #1", text)
        self.assertEqual(sum(args[:3] == ("gh", "pr", "create") for args, _ in self.calls), 1)
        self.assertEqual(Path("first.txt").read_text(encoding="utf-8"), "first\n")

    def test_revert_of_revert_and_reapplication_restore_completion(self):
        for reapply in (False, True):
            with self.subTest(reapply=reapply):
                self.git("switch", "--detach", self.base)
                self.git("cherry-pick", "-x", self.first)
                self.git("revert", "--no-edit", "HEAD")
                ref = self.git("rev-parse", "HEAD")
                self.assertNotIn(self.first, utils.backported_commits(ref))
                if reapply:
                    self.git("cherry-pick", "-x", self.first)
                else:
                    self.git("revert", "--no-edit", "HEAD")
                self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
                present = utils.backported_commits("refs/remotes/origin/release/3.0")
                self.assertIn(self.first, present)
                self.calls.clear()
                self.invoke(relabel.main, ["3.0", "--apply"])
                edits = [args for args, _ in self.calls if args[:3] == ("gh", "pr", "edit")]
                self.assertEqual([args[3] for args in edits], ["1"])

    def test_repeated_revert_targets_require_manual_review_for_removed_and_restored_fixes(self):
        for restored in (False, True):
            with self.subTest(restored=restored):
                self.git("switch", "--detach", self.base)
                self.git("cherry-pick", "-x", self.first)
                backport = self.git("rev-parse", "HEAD")
                self.git("revert", "--no-edit", backport)
                reversal = self.git("rev-parse", "HEAD")
                if restored:
                    self.git("revert", "--no-edit", reversal)
                else:
                    self.git("cherry-pick", "-x", self.first)
                self.git("revert", "--no-edit", backport)
                if restored:
                    self.git("revert", "--no-edit", reversal)
                self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
                self.assertEqual(Path("first.txt").exists(), restored)
                self.calls.clear()
                for function, args in (
                    (generate.main, ["3.0", "1", "--apply"]),
                    (relabel.main, ["3.0", "--apply"]),
                ):
                    with self.subTest(script=function.__module__), self.assertRaisesRegex(ValueError, "manual review"):
                        self.invoke(function, args)
                self.assertEqual(self.mutations(), [])

    def test_reverted_inherited_commit_does_not_count_as_complete(self):
        self.git("switch", "--detach", self.first)
        self.git("revert", "--no-edit", self.first)
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        self.assertNotIn(self.first, utils.backported_commits("HEAD"))
        self.invoke(relabel.main, ["3.0", "--apply"])
        self.assertEqual(self.mutations(), [])

    def reverted_merge(self):
        self.git("switch", "--detach", self.base)
        self.git("cherry-pick", "-x", self.first)
        backport = self.git("rev-parse", "HEAD")
        self.git("switch", "release/3.0")
        self.git("merge", "--no-ff", "-m", "Merge backport", backport)
        merge = self.git("rev-parse", "HEAD")
        self.git("revert", "-m", "1", "--no-edit", merge)
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        return merge

    def test_active_merge_revert_requires_manual_review_before_any_mutation(self):
        self.reverted_merge()
        self.assertFalse(Path("first.txt").exists())
        self.calls.clear()
        for function, args in (
            (generate.main, ["3.0", "1", "--apply"]),
            (relabel.main, ["3.0", "--apply"]),
        ):
            with self.subTest(script=function.__module__), self.assertRaisesRegex(ValueError, "manual review"):
                self.invoke(function, args)
        self.assertEqual(self.mutations(), [])

    def test_reverting_merge_revert_restores_completion(self):
        merge = self.reverted_merge()
        self.git("revert", "--no-edit", "HEAD")
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        self.assertTrue(Path("first.txt").exists())
        present = utils.backported_commits("HEAD")
        self.assertIn(self.first, present)
        self.assertIn(merge, present)
        self.calls.clear()
        self.invoke(relabel.main, ["3.0", "--apply"])
        edits = [args for args, _ in self.calls if args[:3] == ("gh", "pr", "edit")]
        self.assertEqual([args[3] for args in edits], ["1"])

    def test_revert_message_can_reference_an_object_absent_from_history(self):
        self.git("commit", "--allow-empty", "--no-verify", "-m", f"This reverts commit {'a' * 40}.")
        self.assertIn(self.first, utils.backported_commits("HEAD"))

    def test_quoted_revert_bodies_do_not_disable_sources_or_trigger_repeated_target_guard(self):
        for number in (1, 2):
            self.git("commit", "--allow-empty", "--no-verify", "-m", f"Describe prior fixes {number}",
                     "-m", f"Quoted history:\n\nThis reverts commit {self.first}.")
        self.assertIn(self.first, utils.backported_commits("HEAD"))

    def test_squash_merged_revert_and_reapply_subjects_with_pr_suffixes(self):
        for restored in (False, True):
            with self.subTest(restored=restored):
                self.git("switch", "--detach", self.base)
                self.git("cherry-pick", "-x", self.first)
                backport = self.git("rev-parse", "HEAD")
                self.git("revert", "--no-commit", backport)
                self.git("commit", "--no-verify", "-m", 'Revert "first.txt" (#123)',
                         "-m", f"This reverts commit {backport}.")
                if restored:
                    reversal = self.git("rev-parse", "HEAD")
                    self.git("revert", "--no-commit", reversal)
                    self.git("commit", "--no-verify", "-m", 'Reapply "first.txt" (#124)',
                             "-m", f"This reverts commit {reversal}.")
                self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
                self.assertEqual(self.first in utils.backported_commits("HEAD"), restored)
                code, text = self.invoke(generate.main, ["3.0", "1"])
                self.assertEqual(code, 0)
                self.assertEqual("already present" in text, restored)
                self.calls.clear()
                self.invoke(relabel.main, ["3.0", "--apply"])
                edits = [args for args, _ in self.calls if args[:3] == ("gh", "pr", "edit")]
                self.assertEqual([args[3] for args in edits], ["1"] if restored else [])

    def test_merge_revert_annotation_requires_review_even_if_target_object_is_absent(self):
        message = f"This reverts commit {'a' * 40}, reversing\nchanges made to {'b' * 40}."
        self.git("commit", "--allow-empty", "--no-verify", "-m", message)
        with self.assertRaisesRegex(ValueError, "manual review"):
            utils.backported_commits("HEAD")

    def test_merged_backport_without_source_provenance_requires_manual_review(self):
        candidate = {
            "state": "MERGED", "url": "unrecognized", "body": "Clean cherry pick of PR #1",
            "mergeCommit": {"oid": self.base},
        }
        with self.assertRaisesRegex(ValueError, "manual review"):
            utils.active_backports(self.prs[1], [candidate], {self.base})
        self.assertEqual(utils.active_backports(self.prs[1], [candidate], {self.base, self.first}), [candidate])
        self.assertEqual(utils.active_backports(self.prs[1], [candidate], set()), [])

    def test_all_push_urls_are_checked_before_fetch_or_branch_creation(self):
        self.git("remote", "add", "origin", "https://github.com/microsoft/WSL.git")
        self.git("config", "--add", "remote.origin.pushurl", "https://github.com/microsoft/WSL.git")
        self.git("config", "--add", "remote.origin.pushurl", "https://github.com/other/WSL.git")
        self.real_remote_urls = True
        with self.assertRaisesRegex(ValueError, "not a fork"):
            self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(self.mutations(), [])
        self.assertFalse(any(args[:2] == ("git", "fetch") for args, _ in self.calls))

    def local_remote(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        remote = str(Path(directory.name) / "remote.git")
        self.real_run("git", "init", "--bare", remote)
        self.git("remote", "add", "origin", remote)
        self.git("config", "--replace-all", "remote.origin.fetch", "+refs/heads/master:refs/remotes/origin/master")
        self.git("push", "origin", "master", "release/3.0")
        self.real_fetch = True

    def test_explicit_fetch_refreshes_release_with_master_only_refspec(self):
        self.local_remote()
        self.git("branch", "--force", "release/3.0", self.first)
        self.git("push", "origin", "release/3.0")
        self.assertEqual(self.git("rev-parse", "refs/remotes/origin/release/3.0"), self.base)
        ref = utils.fetch_release("origin", "release/3.0")
        self.assertEqual(self.git("rev-parse", ref), self.first)

    def test_deleted_remote_release_cannot_fall_back_to_cached_ref(self):
        self.local_remote()
        self.git("push", "origin", "--delete", "release/3.0")
        self.git("update-ref", "refs/remotes/origin/release/3.0", self.base)
        with self.assertRaises(RuntimeError):
            utils.fetch_release("origin", "release/3.0")

    def test_preview_fetch_does_not_apply_mappings_into_local_branches(self):
        self.local_remote()
        self.git("branch", "--force", "release/3.0", self.first)
        self.git("push", "origin", "release/3.0")
        self.git("branch", "--force", "release/3.0", self.base)
        self.git("config", "--replace-all", "remote.origin.fetch", "+refs/heads/*:refs/heads/*")
        before = self.git("rev-parse", "HEAD")
        self.invoke(relabel.main, ["3.0"])
        self.assertEqual(self.git("rev-parse", "release/3.0"), self.base)
        self.assertEqual(self.git("rev-parse", "refs/remotes/origin/release/3.0"), self.first)
        self.assertEqual(self.git("rev-parse", "HEAD"), before)
        self.assertEqual(self.mutations(), [])

    def test_source_fetch_does_not_apply_mappings_into_local_branches(self):
        self.local_remote()
        self.git("branch", "saved-master", self.base)
        self.git("config", "--replace-all", "remote.origin.fetch", "+refs/heads/master:refs/heads/saved-master")
        code, _ = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertEqual(self.git("rev-parse", "saved-master"), self.base)
        self.assertEqual(sum(args[:3] == ("gh", "pr", "create") for args, _ in self.calls), 1)

    def test_generation_fetches_source_objects_missing_from_release_history(self):
        self.local_remote()
        remote = self.git("remote", "get-url", "origin")
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        with contextlib.ExitStack() as stack:
            previous = Path.cwd()
            stack.callback(os.chdir, previous)
            os.chdir(directory.name)
            self.git("init", "--initial-branch=master")
            self.git("config", "user.name", "Backport Test")
            self.git("config", "user.email", "backport-test@example.invalid")
            self.git("remote", "add", "origin", remote)
            ref = utils.fetch_release("origin", "release/3.0")
            self.git("switch", "--detach", ref)
            self.assertNotEqual(self.real_run("git", "cat-file", "-e", self.first, check=False).returncode, 0)
            code, _ = self.invoke(generate.main, ["3.0", "1", "--apply"])
            self.assertEqual(code, 0)
            self.assertIn(self.first, utils.backported_commits("HEAD"))

    def test_apply_pushes_only_the_branch_when_follow_tags_is_configured(self):
        self.local_remote()
        self.git("tag", "--annotate", "unrelated-tag", self.base, "-m", "Unrelated release tag")
        self.git("config", "push.followTags", "true")
        self.real_push = True
        code, _ = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        head = "refs/heads/cherrypick/release/3.0/pr-1"
        self.assertEqual(self.git("ls-remote", "origin", head).split()[0], self.git("rev-parse", head))
        self.assertEqual(self.git("ls-remote", "--tags", "origin"), "")

    def test_relabel_recognizes_trailers_and_ignores_pr_number_mentions(self):
        self.git("switch", "release/3.0")
        self.git("cherry-pick", "-x", self.first)
        self.git("commit", "--allow-empty", "--no-verify", "-m", "Mention Fix 2 (#2) without backporting it")
        self.git("update-ref", "refs/remotes/origin/release/3.0", "HEAD")
        code, text = self.invoke(relabel.main, ["release/3.0"])
        self.assertEqual(code, 0)
        self.assertIn("/pull/1:", text)
        self.assertNotIn("/pull/2:", text)
        self.assertEqual(self.mutations(), [])
        self.invoke(relabel.main, ["3.0", "--apply"])
        edits = [args for args, _ in self.calls if args[:3] == ("gh", "pr", "edit")]
        self.assertEqual(len(edits), 1)
        self.assertEqual(edits[0][3], "1")
        self.assertIn("backport_3.0", edits[0])
        self.assertIn("backported_3.0", edits[0])
        self.assertFalse(any(args[:3] == ("gh", "label", "create") for args, _ in self.calls))
        self.assertFalse(any(args[:3] == ("gh", "pr", "comment") for args, _ in self.calls))

    def test_inherited_commits_are_skipped_and_relabelled(self):
        self.git("update-ref", "refs/remotes/origin/release/3.0", self.second)
        self.labels = []
        code, text = self.invoke(generate.main, ["3.0", "1", "--apply"])
        self.assertEqual(code, 0)
        self.assertIn("already present", text)
        self.assertEqual(self.mutations(), [])
        self.invoke(relabel.main, ["3.0", "--apply"])
        self.assertEqual(sum(args[:3] == ("gh", "label", "create") for args, _ in self.calls), 1)
        self.assertEqual(sum(args[:3] == ("gh", "pr", "edit") for args, _ in self.calls), 2)

    def test_source_selection_rejects_wrong_base_and_skips_unmerged_labelled_prs(self):
        self.prs[2]["state"] = "OPEN"
        with self.assertRaisesRegex(ValueError, "not merged"):
            self.invoke(generate.main, ["3.0", "2"])
        code, text = self.invoke(generate.main, ["3.0"])
        self.assertEqual(code, 0)
        self.assertIn("Skipping #2: open", text)
        self.assertNotIn("\n#2:", text)
        self.prs[1]["baseRefName"] = "release/2.0"
        with self.assertRaisesRegex(ValueError, "does not target"):
            self.invoke(generate.main, ["3.0", "1"])

    def test_input_and_remote_validation(self):
        for value in ("../3.0", "release/", "3..0", "3.0.", "--all"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                utils.release_branch(value)
        for value in ("https://github.com/other/repo/pull/1", "PR1", "0"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                utils.pr_number(value)
        self.assertEqual(utils.pr_number("https://github.com/microsoft/WSL/pull/42"), 42)
        self.assertEqual(utils.release_branch("release/3.0"), "release/3.0")
        with patch.object(utils, "output", return_value="https://github.com/other/WSL.git"):
            with self.assertRaisesRegex(ValueError, "not a fork"):
                utils.fetch_release("origin", "release/3.0")


if __name__ == "__main__":
    unittest.main()
