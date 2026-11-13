# Release backports

WSL fixes normally land on the default branch (`master`) first. Backport selected
merged PRs to `release/<version>` through separate PRs, then update the labels on
the **original** PRs after the changes reach the release branch.

## Prerequisites

Run the commands below from a WSL checkout using Python 3.9+, Git, and an
authenticated GitHub CLI (`gh`). No extra Python packages are needed.
The scripts target `microsoft/WSL` and default to the `origin` remote. If your
`origin` points to a fork, use `--remote upstream` with a remote pointing to
`microsoft/WSL`; pushing backport branches requires write access there.
When applying, the generator checks every effective push URL as well as the
fetch URL; a separate push destination pointing to a fork is rejected.

The target release branch must already exist. Branch creation and PR merges are
subject to repository rules; admin access does not necessarily grant a bypass.
These scripts never change repository rules.

## Select changes using labels

Use one pair of labels per release, such as:

| Label | Meaning |
| --- | --- |
| `backport_3.0` | The original PR should be backported to `release/3.0`. |
| `backported_3.0` | The original PR's change is present in `release/3.0`. |

The labels for 3.0 already exist. For a new version, create its labels if absent:

```powershell
gh label create backport_3.1 --repo microsoft/WSL --color fbca04 --description "Change should be backported to the release/3.1 branch"
gh label create backported_3.1 --repo microsoft/WSL --color 0e8a16 --description "Change has been backported to the release/3.1 branch"
```

Mark an original PR for a release:

```powershell
gh pr edit 12345 --repo microsoft/WSL --add-label backport_3.0
```

Keep the pending label until the change is actually in the release branch, not
just when the backport PR is opened. A PR can have labels for multiple releases.
To cancel a planned backport, remove its pending label:

```powershell
gh pr edit 12345 --repo microsoft/WSL --remove-label backport_3.0
```

## Preview and create backport PRs

With no PR numbers, the generator selects currently labeled PRs targeting the
default branch. It reports unmerged/abandoned PRs without processing them and
sorts merged PRs by merge time. Explicit PR numbers or WSL PR URLs are also
accepted; these must identify merged PRs on the default branch.

```powershell
# Preview all merged PRs labeled backport_3.0.
python tools\devops\gen_cherrypick_prs.py 3.0

# Preview specific PRs, regardless of labels.
python tools\devops\gen_cherrypick_prs.py release/3.0 12345 12346

# After reviewing the preview, create draft backport PRs.
python tools\devops\gen_cherrypick_prs.py 3.0 --apply --draft
```

Both scripts fetch remote-tracking refs even in preview mode. Without `--apply`,
they do not modify the working tree, create branches, push, create PRs, or edit
labels. Generation requires a clean working tree when applying.
Fetches disable configured ref mappings so they cannot update local branches.

For each selected PR, the generator creates
`cherrypick/release/3.0/pr-<number>` from the current release tip, cherry-picks the
original merge commit with `git cherry-pick -x`, pushes without force, and opens
a PR targeting the release branch. It uses the repository PR template and
records the original PR URL and merge SHA. It skips changes already present in
release history and open/merged backports carrying the original-PR provenance
line, except for recognized reverted backports. Existing local branches are
reported and skipped, never reset, so an earlier attempt does not block later
independent PRs.

Each backport branch is independent. If a later fix depends on an earlier one,
merge the prerequisite backport first, then rerun for the dependent PR. Successful
generation leaves the checkout on the last backport branch.

### Conflicts and retries

On cherry-pick failure, the generator stops before pushing that branch or
creating its PR. Earlier successful backports remain intact. Inspect the error:
an empty pick, a merge commit requiring a mainline, or conflicts require manual
handling.

For conflicts, either abort with `git cherry-pick --abort` or resolve the files,
stage them, and run `git cherry-pick --continue`. For manual backports, retain
the `-x` provenance trailer and the PR-body line
`Backport of https://github.com/microsoft/WSL/pull/<number>` so later runs can
identify the change reliably.

After aborting, the empty local branch still exists and is skipped on subsequent
runs. To retry that PR automatically, switch away, inspect the branch, and delete
only that abandoned branch after confirming it has no work to preserve. After
continuing a resolved pick, push the prepared branch and create its PR manually
with the provenance line before rerunning. Skipping an earlier attempt does not
make its dependent fixes safe to backport.

If push or PR creation fails after the pick succeeded, reuse the prepared branch
manually; the script deliberately will not reset it or force-push it on retry.
Do not overwrite a previous attempt to make an automated rerun succeed.

## Validate and merge

The scripts do **not** build, test, approve, or merge backports. Review the
release-specific diff, update the PR checklist and validation section with
actual results, and wait for the required checks and approvals.

For core component changes, follow [Building and testing WSL](dev-loop.md).
Always build the entire project before running Windows tests, and use an
elevated terminal for the test runner:

```powershell
cmake --build . -- -m
bin\x64\debug\test.bat /name:*UnitTest*
```

Choose tests appropriate to the backported change and your build configuration;
the example subset is not a substitute for feature-specific validation.

## Mark completed backports

After the backport PRs merge:

```powershell
# Preview label changes.
python tools\devops\relabel_backported.py 3.0

# Apply the reviewed changes to the original PRs.
python tools\devops\relabel_backported.py 3.0 --apply
```

The relabeler checks original merge commits and exact
`(cherry picked from commit <sha>)` trailers in release history. It does not infer
completion from similar titles or PR-number mentions. Changes already inherited
when the release branch was created also count as present.
Standard Git revert messages containing `This reverts commit <sha>.` undo
completion; quoted revert bodies inside other commit messages do not.
Reverting that revert or cherry-picking the change again restores completion.
Reverted merged backport PRs do not suppress a new attempt. Arbitrary inverse
patches or edited revert messages cannot be recognized automatically and need
manual review.
If a merged backport remains in history without recognizable source provenance,
the generator stops for manual review rather than assuming it is still active.
Histories that revert the same commit more than once also stop both scripts for
manual review, even if a later revert restores the fix.
An active revert of a merge commit stops both scripts for manual review rather
than counting the merged-side changes as present. Reverting that revert restores
normal detection. This conservative check does not attempt to infer which
changes survived the merge reversal.

For confirmed changes, it adds `backported_3.0` and removes `backport_3.0`.
It creates the completed label if missing, preserves existing label metadata,
and never posts comments. Changes without recognizable provenance stay pending;
review them manually before updating their labels.

## Test the helpers

The helper tests use temporary Git repositories and mocked GitHub operations,
so they never push to WSL or edit live labels:

```powershell
python -m unittest discover -s tools\devops -p test_backports.py
```
