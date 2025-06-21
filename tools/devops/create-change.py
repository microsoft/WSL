import click
import requests
import json
from git import Repo

COMMITTER_EMAIL = 'noreply@microsoft.com'
REPO = 'microsoft/wsl'

@click.command()
@click.argument('repo_path', required=True)
@click.argument('token', required=True)
@click.argument('committer', required=True)
@click.argument('message', required=True)
@click.argument('branch', required=True)
@click.option('--debug', default=False, is_flag=True)
def main(repo_path: str, token: str, committer: str, message: str, branch: str, debug: bool):
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
            'description': 'Automated change',
            'head': branch,
            'base': 'master'
        }

        response = requests.post(f'https://api.github.com/repos/{REPO}/pulls', headers=headers, data=json.dumps(body), timeout=30)
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