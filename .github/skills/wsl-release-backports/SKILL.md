---
name: wsl-release-backports
description: Preview or create WSL release backport PRs and reconcile backport labels. Use when asked to cherry-pick merged WSL PRs to a release branch or manage backport_<version> and backported_<version> labels.
---

# WSL release backports

Read `doc\docs\release-backports.md` before acting. Use the existing Python
helpers rather than inventing a separate cherry-pick or label-management flow.

1. Resolve the requested release version and PR selection. If either is
   ambiguous, ask. Work only in `microsoft/WSL`; inspect the remote and use
   `--remote upstream` if `origin` is a fork. Do not change remotes or repository
   rules to bypass a failure.
   The generator validates all effective push URLs before applying.
2. Preview using `python tools\devops\gen_cherrypick_prs.py <version> [PR numbers]`.
   With no numbers, this selects `backport_<version>` PRs. Report skipped,
   already-backported, and in-progress changes. Preview fetches remote-tracking
   refs but does not change the working tree or GitHub.
3. Use `--apply` only when the user explicitly authorizes creating/pushing
   backport branches and PRs. Prefer `--draft` while validation is outstanding.
   Never discard dirty work, reset existing branches, or force-push a retry.
   The script preserves the PR template and stops on cherry-pick failure;
   retain the failed state and report recovery instructions. Do not continue
   dependent picks until prerequisite backports land.
   Existing local attempts are skipped without resetting them. After aborting,
   inspect the abandoned branch before asking permission to delete it for a
   retry. After continuing a resolved pick, manually push and create its PR with
   the provenance line. Skipped prerequisites still require manual handling.
4. Validate according to the change. For Windows tests, build the entire WSL
   project first and inspect any configured post-build deployment hook before
   running the build. Record exact commands and results in the PR validation
   section; the generator itself does not build or test. Do not mark unchecked
   template items complete without evidence, approve, or merge unless asked.
5. After changes reach the release branch, preview with
   `python tools\devops\relabel_backported.py <version>`. Use `--apply` only with
   explicit authorization to edit labels. Pending labels belong on original
   PRs until the release contains their changes; do not infer completion from
   an open PR, similar title, or PR-number mention.
   Standard Git reverts keep the change pending; revert-of-revert or reapplication
   can restore completion. Review inverse patches without standard provenance
   manually rather than treating historical merge status as sufficient.
   An active revert of a merge commit stops automation for manual review;
   reverting that revert restores normal detection.

For manual conflict resolution, keep the `git cherry-pick -x` trailer and
`Backport of https://github.com/microsoft/WSL/pull/<number>` PR-body line.
Never post comments or review replies unless explicitly requested. Do not put
internal security details or work-item references into public PRs.
