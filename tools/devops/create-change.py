import click
import requests
import json
import sys
from git import Repo

COMMITTER_EMAIL = 'noreply@microsoft.com'
REPO = 'microsoft/wsl'

@click.command()
@click.argument('repo_path', required=True)
@click.argument('token', required=True)
@click.argument('committer', required=True)
@click.argument('message', required=True)
@click.argument('branch', required=True)
@click.argument('target_branch', required=True)
@click.option('--debug', default=False, is_flag=True)
def main(repo_path: str, token: str, committer: str, message: str, branch: str, target_branch: str, debug: bool):
    try:
        repo = Repo(repo_path)

        changed_files = [e.a_path for e in repo.index.diff(None)]

        if not changed_files:
            print('No files changed, skipping')
            return

        print(f'Changed files: {",".join(changed_files)}')


        repo.create_head(branch).checkout()

        with repo.config_writer() as config:
            config.set_value("user", "email", COMMITTER_EMAIL)
            config.set_value("user", "name", committer)

        repo.git.commit('-a', m=message)
        repo.git.push('origin', branch)

        headers = {'Accept': 'application/vnd.github+json', 'Authorization': 'Bearer ' + token}

        body = {
            'title': message,
            'body': 'Automated change',
            'head': branch,
            'base': target_branch
        }

        print(f'POST https://api.github.com/repos/{REPO}/pulls')
        print(f'Request body (token redacted): {json.dumps(body)}')

        response = requests.post(f'https://api.github.com/repos/{REPO}/pulls', headers=headers, data=json.dumps(body), timeout=30)

        print(f'Response status: {response.status_code}')
        # Surface non-secret response headers that GitHub uses to communicate
        # rate limits, request ids, and scope/permission diagnostics.
        for header in ('x-github-request-id', 'x-ratelimit-remaining', 'x-ratelimit-limit',
                       'x-accepted-oauth-scopes', 'x-oauth-scopes', 'x-accepted-github-permissions'):
            value = response.headers.get(header)
            if value is not None:
                print(f'Response header {header}: {value}')

        if not response.ok:
            print(f'Response body: {response.text}', file=sys.stderr)

        response.raise_for_status()

        print(f'Created pull request: {response.json()["html_url"]}')

    except:
        if debug:
            import pdb
            import traceback
            traceback.print_exc()
            pdb.post_mortem()

        raise

if __name__ == '__main__':
    main()