# Copyright (C) Microsoft Corporation. All rights reserved.

"""Preview or create one cherry-pick PR per merged PR, in merge order.

python tools\\devops\\gen_cherrypick_prs.py release/3.0 12345 12346
python tools\\devops\\gen_cherrypick_prs.py 3.0 --apply --draft

Without PR numbers, select backport_<version> PRs on the default branch.
Previews fetch remote-tracking refs but never change branches, push, or create PRs.
Each new branch starts at the release tip; dependent picks may need manual handling.
On conflict, stop and leave the cherry-pick available for manual resolution or abort.
"""

import sys

import click
from git import GitError, Repo

from backport_utils import (
    REPO, active_backports, backported_commits, fetch_release,
    list_prs, pr_body, release_branch, run, source_prs, validate_remote,
)


@click.command(help=__doc__, context_settings={"help_option_names": ["-h", "--help"]})
@click.argument("release")
@click.argument("prs", nargs=-1)
@click.option("--remote", default="origin", show_default=True, help="WSL remote used for fetching and pushing.")
@click.option("--apply", "apply_changes", is_flag=True, help="Cherry-pick, push, and create PRs instead of previewing.")
@click.option("--dry-run", is_flag=True, help="Preview only (the default).")
@click.option("--draft", is_flag=True, help="Create draft PRs.")
@click.pass_obj
def main(repo, release, prs, remote, apply_changes, dry_run, draft):
    if apply_changes and dry_run:
        raise click.UsageError("--apply and --dry-run are mutually exclusive.")
    branch = release_branch(release)
    prs = source_prs(branch, prs)
    if not prs:
        print("No merged PRs selected.")
        return
    if repo is not None:
        return generate(repo, branch, prs, remote, apply_changes, draft)
    with Repo(".", search_parent_directories=True) as repo:
        return generate(repo, branch, prs, remote, apply_changes, draft)


def generate(repo, branch, prs, remote, apply_changes, draft):
    if apply_changes and repo.is_dirty(untracked_files=True):
        raise ValueError("Working tree must be clean before applying cherry-picks.")
    if apply_changes:
        validate_remote(repo, remote, push=True)
    ref = fetch_release(repo, remote, branch)
    if apply_changes:
        repo.git.fetch("--no-tags", "--refmap=", remote, f"refs/heads/{prs[0]['baseRefName']}")
    picked = backported_commits(repo, ref)
    candidates = list_prs(branch)

    for pr in prs:
        number = pr["number"]
        sha = pr["mergeCommit"]["oid"]
        if sha in picked:
            print(f"Skipping #{number}: already present in {branch}.")
            continue
        existing = active_backports(repo, pr, candidates, picked)
        if existing:
            print(f"Skipping #{number}: existing backport " + ", ".join(item["url"] for item in existing))
            continue
        head = f"cherrypick/{branch}/pr-{number}"
        if head in repo.heads:
            print(f"Skipping #{number}: existing local branch {head}; finish or abandon that attempt manually.")
            continue
        title = f"[{branch}] {pr['title']} (#{number})"
        body = pr_body(repo, pr, branch)
        print(f"\n#{number}: {sha} -> {head}\nTitle: {title}\n{body}")
        if not apply_changes:
            continue

        # Never reset an existing branch or force-push a prior attempt.
        repo.create_head(head, ref).checkout()
        try:
            repo.git.cherry_pick("-x", sha)
        except GitError as error:
            print(error, file=sys.stderr)
            print("Stopped before push/PR creation. Resolve the cherry-pick or run git cherry-pick --abort.",
                  file=sys.stderr)
            raise click.exceptions.Exit(2) from error
        repo.git.push("--no-follow-tags", "--set-upstream", remote, f"HEAD:refs/heads/{head}")
        command = [
            "gh", "pr", "create", "--repo", REPO, "--base", branch, "--head", head,
            "--title", title, "--body-file", "-",
        ]
        if draft:
            command.append("--draft")
        print(run(*command, input=body).stdout.strip())

    if not apply_changes:
        print("\nPreview only. Use --apply to create the backport PRs.")
    return 0


if __name__ == "__main__":
    try:
        main()
    except (GitError, RuntimeError, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
