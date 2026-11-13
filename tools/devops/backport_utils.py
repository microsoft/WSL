# Copyright (C) Microsoft Corporation. All rights reserved.

"""Shared helpers for the WSL release scripts (Python 3.9+, git, and authenticated gh)."""

import json
import re
import subprocess
from pathlib import Path


REPO = "microsoft/WSL"
PR_FIELDS = "number,title,url,state,mergedAt,mergeCommit,body,baseRefName,labels"


def run(*args, input=None, check=True):
    result = subprocess.run(args, input=input, capture_output=True, text=True, encoding="utf-8")
    if check and result.returncode:
        raise RuntimeError(f"{' '.join(args)} failed:\n{result.stdout}{result.stderr}")
    return result


def output(*args):
    return run(*args).stdout.strip()


def release_branch(value):
    version = value.removeprefix("release/")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", version) or ".." in version or version.endswith("."):
        raise ValueError(f"Invalid release version: {value}")
    return f"release/{version}"


def pr_number(value):
    match = re.fullmatch(r"(?:#)?([1-9][0-9]*)", value)
    if not match:
        match = re.fullmatch(r"https://github\.com/microsoft/WSL/pull/([1-9][0-9]*)/?", value, re.IGNORECASE)
    if not match:
        raise ValueError(f"Not a WSL PR number or URL: {value}")
    return int(match.group(1))


def default_branch():
    return json.loads(output("gh", "repo", "view", REPO, "--json", "defaultBranchRef"))["defaultBranchRef"]["name"]


def list_prs(base, label=None):
    args = ["gh", "pr", "list", "--repo", REPO, "--base", base, "--state", "all",
            "--limit", "1000", "--json", PR_FIELDS]
    if label:
        args += ["--label", label]
    prs = json.loads(output(*args))
    if len(prs) == 1000:
        raise RuntimeError("PR query reached its 1000-result limit; narrow the selection before proceeding.")
    return [pr for pr in prs if not label or any(item["name"] == label for item in pr["labels"])]


def source_prs(branch, numbers):
    base = default_branch()
    if numbers:
        prs = [
            json.loads(output("gh", "pr", "view", str(number), "--repo", REPO, "--json", PR_FIELDS))
            for number in dict.fromkeys(pr_number(value) for value in numbers)
        ]
    else:
        prs = list_prs(base, f"backport_{branch.removeprefix('release/')}")
    merged = []
    for pr in prs:
        if pr["baseRefName"] != base:
            raise ValueError(f"PR #{pr['number']} does not target {base}.")
        if pr["state"] != "MERGED":
            if numbers:
                raise ValueError(f"PR #{pr['number']} is not merged.")
            print(f"Skipping #{pr['number']}: {pr['state'].lower()}.")
            continue
        if not pr["mergedAt"] or not pr["mergeCommit"]:
            raise ValueError(f"PR #{pr['number']} has no merge commit or merge date.")
        merged.append(pr)
    return sorted(merged, key=lambda pr: pr["mergedAt"])


def validate_remote(remote, push=False):
    args = ["git", "remote", "get-url"]
    if push:
        args += ["--push", "--all"]
    urls = output(*args, remote).splitlines()
    for url in urls:
        if not re.fullmatch(r"(?:https://github\.com/|git@github\.com:)microsoft/WSL(?:\.git)?/?", url, re.IGNORECASE):
            raise ValueError(f"Remote {remote} must point to {REPO}, not a fork: {url}")
    if not urls:
        raise ValueError(f"Remote {remote} has no {'push' if push else 'fetch'} URL.")


def fetch_release(remote, branch):
    validate_remote(remote)
    print(f"Fetching {remote} (remote-tracking refs only).")
    ref = f"refs/remotes/{remote}/{branch}"
    output("git", "fetch", "--no-tags", "--refmap=", remote, f"+refs/heads/{branch}:{ref}")
    output("git", "rev-parse", "--verify", ref)
    return ref


def backported_commits(ref):
    fields = output("git", "log", "--topo-order", "--format=%H %P%x00%B%x00", ref).split("\0")
    records = [(fields[index].split(), fields[index + 1]) for index in range(0, len(fields) - 1, 2)]
    parents = {identity[0]: identity[1:] for identity, _ in records}
    present, reverted = set(), set()
    # Walk newest first: an active revert disables its target; reverting that
    # revert disables the reversal instead, restoring the original commit.
    for identity, message in records:
        sha = identity[0]
        sources = re.findall(r"^\(cherry picked from commit ([0-9a-f]{40})\)$", message, re.MULTILINE)
        if sha in reverted or any(source in reverted for source in sources):
            continue
        present.add(sha)
        present.update(sources)
        targets = re.findall(r"^This reverts commit ([0-9a-f]{40})[.,]", message, re.MULTILINE)
        merge_targets = set(re.findall(r"^This reverts commit ([0-9a-f]{40}), reversing\b", message, re.MULTILINE))
        for target in targets:
            if len(parents.get(target, [])) > 1 or target in merge_targets:
                raise ValueError(
                    f"Release history contains active revert {sha} of merge {target}; "
                    "manual review is required before generating backports or updating labels."
                )
        reverted.update(targets)
    return present


def active_backports(pr, candidates, present):
    marker = re.compile(
        rf"^(?:Backport of https://github\.com/microsoft/WSL/pull/{pr['number']}(?!\d)"
        rf"|Clean cherry pick of PR #{pr['number']}(?!\d))",
        re.MULTILINE | re.IGNORECASE,
    )
    return [
        item for item in candidates
        if marker.search(item["body"] or "") and (
            item["state"] == "OPEN" or (
                item["state"] == "MERGED" and item.get("mergeCommit")
                and item["mergeCommit"]["oid"] in present
            )
        )
    ]


def ensure_completed_label(name, branch):
    pages = json.loads(output("gh", "api", "--paginate", "--slurp", f"repos/{REPO}/labels?per_page=100"))
    if not any(label["name"] == name for page in pages for label in page):
        output("gh", "label", "create", name, "--repo", REPO, "--color", "0e8a16",
               "--description", f"Change has been backported to the {branch} branch")


def pr_body(pr, branch):
    root = Path(output("git", "rev-parse", "--show-toplevel"))
    body = (root / ".github" / "pull_request_template.md").read_text(encoding="utf-8")
    sections = {
        "## Summary of the Pull Request":
            f"Backport of {pr['url']} to `{branch}`.\n\n{pr['title']}",
        "## Detailed Description of the Pull Request / Additional comments":
            f"Original merge commit: `{pr['mergeCommit']['oid']}`.",
        "## Validation Steps Performed":
            "Cherry-picked with `git cherry-pick -x`.\n\nBuild and tests: not run by this script.",
    }
    for heading, content in sections.items():
        if heading not in body.splitlines():
            raise ValueError(f"PR template is missing {heading!r}.")
        body = body.replace(heading + "\n", heading + "\n\n" + content + "\n", 1)
    return body
