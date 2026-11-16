# Copyright (C) Microsoft Corporation. All rights reserved.

"""Preview or update labels for PRs whose commits reached a release branch.

python tools\\devops\\relabel_backported.py 3.0
python tools\\devops\\relabel_backported.py release/3.0 --apply

Fetches remote-tracking refs. Recognizes original commits in release history and
exact git cherry-pick -x trailers, not title/PR-number guesses. Never posts comments.
"""

import sys

import click
from git import GitError, Repo

from backport_utils import (
    REPO, backported_commits, ensure_completed_label, fetch_release,
    output, release_branch, source_prs,
)


@click.command(help=__doc__, context_settings={"help_option_names": ["-h", "--help"]})
@click.argument("release")
@click.option("--remote", default="origin", show_default=True, help="WSL remote to fetch.")
@click.option("--apply", "apply_changes", is_flag=True, help="Update labels instead of previewing.")
@click.pass_obj
def main(repo, release, remote, apply_changes):
    branch = release_branch(release)
    version = branch.removeprefix("release/")
    prs = source_prs(branch, [])
    if not prs:
        print("No merged PRs with the backport label.")
        return
    if repo is not None:
        return relabel(repo, branch, version, prs, remote, apply_changes)
    with Repo(".", search_parent_directories=True) as repo:
        return relabel(repo, branch, version, prs, remote, apply_changes)


def relabel(repo, branch, version, prs, remote, apply_changes):
    ref = fetch_release(repo, remote, branch)
    picked = backported_commits(repo, ref)
    pending, completed = f"backport_{version}", f"backported_{version}"
    matches = [pr for pr in prs if pr["mergeCommit"]["oid"] in picked]
    if apply_changes and matches:
        ensure_completed_label(completed, branch)

    for pr in matches:
        print(f"{pr['url']}: {pending} -> {completed}")
        if apply_changes:
            output("gh", "pr", "edit", str(pr["number"]), "--repo", REPO,
                   "--add-label", completed, "--remove-label", pending)
    if not matches:
        print("No selected commits found in release history.")
    elif not apply_changes:
        print("Preview only. Use --apply to update labels.")
    return 0


if __name__ == "__main__":
    try:
        main()
    except (GitError, RuntimeError, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
