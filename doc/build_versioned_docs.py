#!/usr/bin/env python3
# Copyright (C) Microsoft Corporation. All rights reserved.
"""Build the docs site, versioning only the API reference.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

# Paths to the mkdocs configs, relative to the repo root.
CONFIG_MAIN = "doc/mkdocs.yml"        # the whole site, single (current) version
CONFIG_API = "doc/mkdocs-api.yml"     # the API reference, versioned by mike
# Default lower bound: only tags >= this version are built into the site.
DEFAULT_MIN_VERSION = "3.0.0"


def run(args: list[str], capture: bool = False) -> str:
    """Run a command, raising on failure. Returns stdout when capture=True."""
    result = subprocess.run(
        args,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    return (result.stdout or "") if capture else ""


def git(args: list[str], capture: bool = False) -> str:
    return run(["git", *args], capture=capture)


def _mike_executable() -> str:
    """Locate the mike console script (``python -m mike`` is not supported)."""
    found = shutil.which("mike")
    if found:
        return found
    base = Path(sys.executable).parent
    for cand in (base / "Scripts" / "mike.exe", base / "mike.exe", base / "mike"):
        if cand.exists():
            return str(cand)
    sys.exit("mike not found on PATH; run: pip install -r doc/requirements.txt")


_MIKE = None


def mike(args: list[str]) -> None:
    global _MIKE
    if _MIKE is None:
        _MIKE = _mike_executable()
    run([_MIKE, *args])


def mkdocs(args: list[str]) -> None:
    # mkdocs supports module execution, so this works without PATH lookups.
    run([sys.executable, "-m", "mkdocs", *args])


def parse_version(text: str) -> tuple[int, ...] | None:
    """Parse a dotted numeric version (ignoring any pre-release suffix).

    Returns a comparable tuple, or None if the string isn't version-like.
    """
    m = re.match(r"^(\d+)(?:\.(\d+))?(?:\.(\d+))?", text)
    if not m:
        return None
    return tuple(int(p) if p else 0 for p in m.groups())


def tag_has_api_docs(tag: str) -> bool:
    """True if the given tag contains the API-reference config (so it builds)."""
    try:
        git(["cat-file", "-e", f"{tag}:{CONFIG_API}"], capture=True)
        return True
    except subprocess.CalledProcessError:
        return False


def collect_release_tags(min_version: tuple[int, ...]) -> list[tuple[str, tuple[int, ...]]]:
    """Return [(tag, version_tuple), ...] for qualifying tags, oldest -> newest.

    A tag qualifies when it matches ``v<MAJOR>.<MINOR>.<PATCH>``, parses to a
    version >= ``min_version``, and actually contains the API-reference docs.
    """
    raw = git(["tag", "--list", "v*", "--sort=version:refname"], capture=True)
    tags: list[tuple[str, tuple[int, ...]]] = []
    for tag in (line.strip() for line in raw.splitlines()):
        if not tag:
            continue
        version = parse_version(tag.lstrip("v"))
        if version is None or version < min_version:
            continue
        if not tag_has_api_docs(tag):
            continue
        tags.append((tag, version))
    return tags


def deploy(ref: str, version: str, alias: str | None = None) -> None:
    """Build the API reference at ``ref`` and publish it (locally) as ``version``."""
    git(["checkout", "-q", ref])
    # --alias-type=copy keeps the output symlink-free so it extracts and serves
    # correctly on any static host (including Windows checkouts).
    args = ["deploy", "--config-file", CONFIG_API, "--alias-type=copy"]
    if alias:
        args += ["--update-aliases", version, alias]
    else:
        args += [version]
    mike(args)


def export_branch(branch: str, dest: Path) -> None:
    """Export ``branch``'s tree into ``dest`` (replacing any existing contents)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    archive = dest.parent / f"{dest.name}.tar"
    try:
        with archive.open("wb") as fh:
            subprocess.run(["git", "archive", branch], check=True, stdout=fh)
        with tarfile.open(archive) as tf:
            tf.extractall(dest)
    finally:
        archive.unlink(missing_ok=True)


def ensure_git_identity() -> None:
    """mike commits to a local branch, so a git identity must be set."""
    for key, default in (("user.name", "docs-builder"),
                         ("user.email", "docs-builder@localhost")):
        try:
            git(["config", key], capture=True)
        except subprocess.CalledProcessError:
            git(["config", key, default])


def order_versions_dev_last(api_dir: Path) -> None:
    """Move the in-progress ``dev`` build to the end of the version selector.

    mike sorts ``versions.json`` so non-numeric versions (``dev``) come first;
    the Material/mike selector renders the file in array order, so we reorder it
    so release versions appear first (newest -> oldest) and ``dev`` is listed
    last. The default landing version is unaffected (set via ``mike set-default``).
    """
    versions_file = api_dir / "versions.json"
    if not versions_file.exists():
        return
    versions = json.loads(versions_file.read_text(encoding="utf-8"))
    # Stable sort: keeps mike's newest-first release order, pushes "dev" to the end.
    versions.sort(key=lambda entry: entry.get("version") == "dev")
    versions_file.write_text(json.dumps(versions, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default="public",
                        help="output directory for the built site (default: public)")
    parser.add_argument("--min-version", default=DEFAULT_MIN_VERSION,
                        help=f"only build API-reference tags >= this version "
                             f"(default: {DEFAULT_MIN_VERSION})")
    options = parser.parse_args()

    min_version = parse_version(options.min_version)
    if min_version is None:
        parser.error(f"invalid --min-version: {options.min_version!r}")

    output = Path(options.output)

    # Remember the starting point so we can build "dev" from it and return here.
    start_sha = git(["rev-parse", "HEAD"], capture=True).strip()

    # 1) Build the main site once, from the current branch. This is the whole
    #    documentation set minus the (separately versioned) API reference.
    print("Building main documentation site (single version)...")
    mkdocs(["build", "--clean", "--config-file", CONFIG_MAIN,
            "--site-dir", str(output.resolve())])

    ensure_git_identity()

    # Start from a clean local gh-pages branch every run; it is never pushed and
    # serves only as mike's scratch space for the API reference.
    subprocess.run(["git", "branch", "-D", "gh-pages"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    tags = collect_release_tags(min_version)
    latest_tag = tags[-1][0] if tags else None

    print(f"\nAPI reference -- minimum version: {options.min_version}")
    if tags:
        print("Building API-reference versions: " + ", ".join(t for t, _ in tags))
    else:
        print("No qualifying release tags found; building API reference 'dev' only.")

    try:
        # 2) Build every qualifying release of the API reference; the newest also
        #    gets the "latest" alias.
        for tag, _version in tags:
            name = tag.lstrip("v")
            deploy(tag, name, "latest" if tag == latest_tag else None)

        # Build the current branch as the in-progress "dev" version.
        deploy(start_sha, "dev")

        # Choose the version visitors land on by default.
        mike(["set-default", "--config-file", CONFIG_API,
              "latest" if latest_tag else "dev"])
    finally:
        # Always return to the original checkout, even on failure.
        git(["checkout", "-q", start_sha])

    # 3) Export the versioned API reference into <output>/api-reference/.
    export_branch("gh-pages", output / "api-reference")

    # mike lists "dev" first; reorder so releases come first and "dev" is last.
    order_versions_dev_last(output / "api-reference")

    print(f"\nBuilt docs into '{output}':")
    for child in sorted(output.iterdir()):
        suffix = os.sep if child.is_dir() else ""
        print(f"  {child.name}{suffix}")
    print(f"\nVersioned API reference is under '{output / 'api-reference'}'.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
