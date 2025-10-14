# pip install click gitpython

# Usage: 

# python tools\devops\find-release.py src/windows/wsl/main.cpp 10
# python tools\devops\find-release.py  --commit 4ec2def9f3f588c06308cb31cb758e5369761aa5

import click
import re

from git import Repo, Commit, Tag

@click.command()
@click.argument('ref', required=True, type=str)
@click.argument('line', default=None, required=False, type=int)
@click.option('--commit', is_flag=True)
def main(ref: str, line: int, commit: bool):
    repo = Repo('.')

    repo.remote('origin').fetch()
    tags = list_tags(repo)
    
    if commit:
        change = repo.commit(ref)
        click.secho(f'{ref}: {find_tag_for_commit(repo, tags, change)}', fg='green', bold=True)
    else:
    
        for entry in repo.blame_incremental('HEAD', ref):
            if line >= entry.linenos.start and line <= entry.linenos.stop:
                click.secho(f'Changed in {find_tag_for_commit(repo, tags, entry.commit)} by {entry.commit.hexsha}', fg='green', bold=True)

                print(repo.git.diff(entry.commit, entry.commit.parents[0], ref) + '\n')
    

def list_tags(repo: Repo) -> list:
    return [e for e in sorted(repo.tags, key=lambda e: e.path) if re.match('refs/tags/[0-9]+\\.[0-9]+\\.[0-9]+', e.path)]

def find_tag_for_commit(repo: Repo, tags: list, commit: Commit) -> str:
    for e in tags:
        merge_bases = repo.merge_base(e, commit)
        
        if any(e == commit for e in merge_bases):
            return e.path.replace('refs/tags/', '')

    return "[No tag found]"

if __name__ == '__main__':
    main()