# Copyright (C) Microsoft Corporation. All rights reserved.

"""Preview or create one cherry-pick PR per merged PR, in merge order.

python tools\\devops\\gen_cherrypick_prs.py release/3.0 12345 12346
python tools\\devops\\gen_cherrypick_prs.py 3.0 --apply --draft

Without PR numbers, select backport_<version> PRs on the default branch.
Previews fetch remote-tracking refs but never change branches, push, or create PRs.
Each new branch starts at the release tip; dependent picks may need manual handling.
On conflict, stop and leave the cherry-pick available for manual resolution or abort.
"""

import argparse
import sys

from backport_utils import (
    REPO, active_backports, backported_commits, fetch_release,
    list_prs, output, pr_body, release_branch, run, source_prs, validate_remote,
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("release", help="Release version or branch, e.g. 3.0 or release/3.0")
    parser.add_argument("prs", nargs="*", help="Merged WSL PR numbers or URLs; default: backport_<version> label")
    parser.add_argument("--remote", default="origin", help="WSL remote used for fetching and pushing (default: origin)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--apply", action="store_true", help="Cherry-pick, push, and create PRs instead of previewing")
    mode.add_argument("--dry-run", action="store_true", help="Preview only (the default)")
    parser.add_argument("--draft", action="store_true", help="Create draft PRs")
    args = parser.parse_args(argv)

    branch = release_branch(args.release)
    prs = source_prs(branch, args.prs)
    if not prs:
        print("No merged PRs selected.")
        return 0
    if args.apply and output("git", "status", "--porcelain"):
        raise ValueError("Working tree must be clean before applying cherry-picks.")
    if args.apply:
        validate_remote(args.remote, push=True)
    ref = fetch_release(args.remote, branch)
    if args.apply:
        output("git", "fetch", "--no-tags", "--refmap=", args.remote, f"refs/heads/{prs[0]['baseRefName']}")
    picked = backported_commits(ref)
    candidates = list_prs(branch)

    for pr in prs:
        number = pr["number"]
        sha = pr["mergeCommit"]["oid"]
        if sha in picked:
            print(f"Skipping #{number}: already present in {branch}.")
            continue
        existing = active_backports(pr, candidates, picked)
        if existing:
            print(f"Skipping #{number}: existing backport " + ", ".join(item["url"] for item in existing))
            continue
        head = f"cherrypick/{branch}/pr-{number}"
        result = run("git", "show-ref", "--verify", "--quiet", f"refs/heads/{head}", check=False)
        if result.returncode == 0:
            print(f"Skipping #{number}: existing local branch {head}; finish or abandon that attempt manually.")
            continue
        if result.returncode != 1:
            raise RuntimeError(f"Cannot check local branch {head}:\n{result.stderr}")
        title = f"[{branch}] {pr['title']} (#{number})"
        body = pr_body(pr, branch)
        print(f"\n#{number}: {sha} -> {head}\nTitle: {title}\n{body}")
        if not args.apply:
            continue

        # Never reset an existing branch or force-push a prior attempt.
        output("git", "switch", "--create", head, ref)
        result = run("git", "cherry-pick", "-x", sha, check=False)
        if result.returncode:
            print(result.stdout + result.stderr, file=sys.stderr)
            print("Stopped before push/PR creation. Resolve the cherry-pick or run git cherry-pick --abort.",
                  file=sys.stderr)
            return 2
        output("git", "push", "--set-upstream", args.remote, f"HEAD:refs/heads/{head}")
        command = [
            "gh", "pr", "create", "--repo", REPO, "--base", branch, "--head", head,
            "--title", title, "--body-file", "-",
        ]
        if args.draft:
            command.append("--draft")
        print(run(*command, input=body).stdout.strip())

    if not args.apply:
        print("\nPreview only. Use --apply to create the backport PRs.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
