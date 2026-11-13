# Copyright (C) Microsoft Corporation. All rights reserved.

"""Preview or update labels for PRs whose commits reached a release branch.

python tools\\devops\\relabel_backported.py 3.0
python tools\\devops\\relabel_backported.py release/3.0 --apply

Fetches remote-tracking refs. Recognizes original commits in release history and
exact git cherry-pick -x trailers, not title/PR-number guesses. Never posts comments.
"""

import argparse
import sys

from backport_utils import (
    REPO, backported_commits, ensure_completed_label, fetch_release,
    output, release_branch, source_prs,
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("release", help="Release version or branch, e.g. 3.0 or release/3.0")
    parser.add_argument("--remote", default="origin", help="WSL remote to fetch (default: origin)")
    parser.add_argument("--apply", action="store_true", help="Update labels instead of previewing")
    args = parser.parse_args(argv)
    branch = release_branch(args.release)
    version = branch.removeprefix("release/")
    prs = source_prs(branch, [])
    if not prs:
        print("No merged PRs with the backport label.")
        return 0
    ref = fetch_release(args.remote, branch)
    picked = backported_commits(ref)
    pending, completed = f"backport_{version}", f"backported_{version}"
    matches = [pr for pr in prs if pr["mergeCommit"]["oid"] in picked]
    if args.apply and matches:
        ensure_completed_label(completed, branch)

    for pr in matches:
        print(f"{pr['url']}: {pending} -> {completed}")
        if args.apply:
            output("gh", "pr", "edit", str(pr["number"]), "--repo", REPO,
                   "--add-label", completed, "--remove-label", pending)
    if not matches:
        print("No selected commits found in release history.")
    elif not args.apply:
        print("Preview only. Use --apply to update labels.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
