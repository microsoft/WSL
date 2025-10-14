# pip install click requests gitpython

import click
import requests
import sys
import re
import os
import backoff
import functools
from git import Repo
from urllib.parse import urlparse

@click.command()
@click.argument('version', required=True)
@click.argument('assets', default=None, nargs=-1)
@click.option('--previous', default=None)
@click.option('--max-message-lines', default=1)
@click.option('--publish', is_flag=True, default=False)
@click.option('--no-fetch', is_flag=True, default=False)
@click.option('--github-token', default=None)
@click.option('--use-current-ref', is_flag=True, default=False)
@click.option('--auto-release-notes', is_flag=True, default=False)
def main(version: str, previous: str, max_message_lines: int, publish: bool, assets: list, no_fetch: bool, github_token: str, use_current_ref: bool, auto_release_notes: bool):
    if publish:
        # Click provides an empty tuple when no assets are passed. Guard against both
        # an explicit None (older Click versions / direct invocation) and an empty
        # collection so we do not accidentally create a release without payload.
        if not assets:
            raise RuntimeError('--publish requires at least one asset')

        if github_token is None:
            raise RuntimeError('--publish requires --github_token')

    for e in assets:
        if not os.path.exists(e):
            raise RuntimeError(f'Asset not found: {e}')

    if previous is None:
        previous = get_previous_release(parse_tag(version))

    current_ref = '<current-commit>' if use_current_ref else version
    print(f'Creating release notes for: {previous} -> {current_ref}', file=sys.stderr)

    changes = ''

    if not auto_release_notes:
        for e in get_change_list(None if use_current_ref else version, previous, not no_fetch):

            # Detect attached github issues
            issues = find_github_issues(e.message)
            pr_description, pr_number = get_github_pr_message(github_token, e.message)
            if pr_description is not None:
                issues = issues.union(find_github_issues(pr_description))

            if github_token is not None:
                issues = filter_github_issues(issues, github_token)

            if len(issues) > 1:
                print(f'WARNING: found more than 1 github issues in message: {e.message}. Issues: {issues}', file=sys.stderr)

            message = e.message[:-1] if e.message.endswith('\n') else e.message

            # Shrink the message if it's too long
            lines = message.split('\n')
            message = '\n'.join([e for e in lines if e][:max_message_lines])

            # Get rid of the github PR #
            if pr_number is not None:
                message = message.replace(f'(#{pr_number})', '')

            # Append to the changes (chr(92) == '\n')
            message = f'{message.replace(chr(92), "")} (solves {",".join(issues)})' if issues else message
            changes += f'* {message}\n'

    if publish:
        publish_release(version, changes, assets, auto_release_notes, github_token)
    else:
        print(f'\n{changes}')

@backoff.on_exception(backoff.expo, (requests.exceptions.Timeout, requests.exceptions.ConnectionError, requests.exceptions.RequestException), max_time=600)
def get_github_pr_message(token: str, message: str) -> str:
    match = re.search(r'\(#([0-9]+)\)', message)
    if match is None:
        print(f'Warning: failed to extract GitHub PR number from message: {message}')
        return None, None

    pr_number = match.group(1)
    headers = {'Accept': 'application/vnd.github+json',
               'Authorization': 'Bearer ' + token,
               'X-GitHub-Api-Version': '2022-11-28'}

    response = requests.get(f'https://api.github.com/repos/microsoft/wsl/pulls/{pr_number}', timeout=30, headers=headers)
    response.raise_for_status()

    return response.json()['body'], pr_number


def parse_tag(tag: str) -> list:
    version = tag.split('.')
    if len(version) != 3:
        raise RuntimeError(f'Unexpected tag: {version}')

    return tuple(int(e) for e in version)

def get_previous_release(version: tuple) -> str:
    response = requests.get('https://api.github.com/repos/Microsoft/WSL/releases');
    response.raise_for_status()

    # Find the most recent release with a lower version number than this one
    versions = [parse_tag(e['tag_name']) for e in response.json()]
    previous_versions = [e for e in versions if e < version]

    if not previous_versions:
        raise RuntimeError(f'No previous found on GitHub. Response: {response.json()}')

    return '.'.join(str(e) for e in max(previous_versions))

def find_github_issues(message: str):
    # Look for urls first
    urls = [urlparse(e) for e in re.findall(r"https?://[^\s^\)]+", message)]

    issue_urls = [e for e in urls if e.hostname == 'github.com' and e.path.lower().startswith('/microsoft/wsl/issues/')]

    issues = set(['#' + e.path.split('/')[-1] for e in issue_urls])

    # Then add issue numbers
    for e in re.findall(r"#\d+", message):
        issues.add(e)

    return issues

def filter_github_issues(issues: list, token: str) -> list:

    @functools.cache
    def is_pr(number: str):
        headers = {
                   'Accept': 'application/vnd.github+json',
                   'Authorization': 'Bearer ' + token,
                   'X-GitHub-Api-Version': '2022-11-28'
                  }

        response = requests.get(f'https://api.github.com/repos/microsoft/wsl/issues/{number}', timeout=30, headers=headers)
        response.raise_for_status()

        return response.json().get('pull_request') is not None

    return [e for e in issues if not is_pr(e.replace('#', ''))]


def get_change_list(version: str, previous: str, fetch: bool) -> list:
    repo = Repo('.')

    # Fetch origin first
    if fetch and version is not None:
        repo.remote('origin').fetch(previous)

    # Find both current and previous version tags
    previous_tag = repo.tag(previous)

    # Set current ref
    current_ref = repo.tag(version) if version is not None else repo.head

    # Find common root between tags
    merge_bases = repo.merge_base(previous_tag.commit, current_ref)
    if len(merge_bases) == 0:
        raise RuntimeError(f'No merge base found between {version} and {previous}')
    elif len(merge_bases) > 1:
        raise RuntimeError(f'Multiple merge bases found between {version} and {previous}')

    # List commits between tags
    for e in repo.iter_commits(rev=current_ref):
        if e == merge_bases[0]:
            return

        yield e

    raise RuntimeError(f'Tag {previous} is not an ancestor of {version}')


@backoff.on_exception(backoff.expo, (requests.exceptions.Timeout, requests.exceptions.ConnectionError, requests.exceptions.RequestException), max_time=600)
def publish_release(version: str, changes: str, assets: list, auto_release_notes: bool, token: str):
    print(f'Creating private GitHub release for: {version}', file=sys.stderr)

    # First create the release
    headers = {'Accept': 'application/vnd.github+json',
               'Authorization': 'Bearer ' + token,
               'X-GitHub-Api-Version': '2022-11-28'}

    content = {'tag_name': version,
               'target_commitish': 'master',
               'name': version,
               "draft":True ,
               'prerelease':True ,
               'generate_release_notes': auto_release_notes}

    if changes:
        content['body'] = changes

    response = requests.post('https://api.github.com/repos/microsoft/wsl/releases', json=content, headers=headers)
    response.raise_for_status()

    release = response.json()
    print(f'Created release: {release["url"]}', file=sys.stderr)

    for asset in assets:
        with open(asset, 'rb') as asset_content:
            asset_size = os.path.getsize(asset)

            # Append asset to the release assets
            headers['Content-Type'] = 'application/octet-stream'

            response = requests.post(f'https://uploads.github.com/repos/microsoft/wsl/releases/{release["id"]}/assets?name={os.path.basename(asset)}', headers=headers, data=asset_content)
            response.raise_for_status()

            print(f'Attached asset: {asset} to release: {response.json()["url"]}', file=sys.stderr)

    print(f'The release has been created. Navigate to {release["html_url"]} to edit the release notes and publish it', file=sys.stderr)

if __name__ == '__main__':
    main()