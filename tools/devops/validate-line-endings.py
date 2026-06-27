import click
import os.path
from git import Repo

EXTENSIONS = ['.cpp', '.h', '.hpp', '.idl', '.resw']

def is_source_file(path: str) -> bool:
    return any(path.casefold().endswith(e) for e in EXTENSIONS)

def has_crlf_mismatch(content: bytes) -> bool:
    # Strip all CRLF pairs, then any remaining lone '\n' or '\r' is a mismatch.
    stripped = content.replace(b'\r\n', b'')
    return b'\n' in stripped or b'\r' in stripped

def to_crlf(content: bytes) -> bytes:
    # Normalize every line ending (CRLF, lone CR, lone LF) to a single CRLF.
    return content.replace(b'\r\n', b'\n').replace(b'\r', b'\n').replace(b'\n', b'\r\n')

@click.command()
@click.argument('repo', required=True, type=click.Path(exists=True))
@click.option('--fix', is_flag=True, help='Convert mismatching files to CRLF line endings.')
def main(repo: str, fix: bool):
    repo = Repo(repo, search_parent_directories=True)

    tracked = repo.git.ls_files('-z').split('\0')
    source_files = [os.path.join(repo.working_tree_dir, e) for e in tracked if e and is_source_file(e)]

    mismatches = []
    for e in source_files:
        with open(e, 'rb') as fd:
            content = fd.read()

        if not has_crlf_mismatch(content):
            continue

        mismatches.append(e)
        if fix:
            with open(e, 'wb') as fd:
                fd.write(to_crlf(content))

    if not mismatches:
        click.secho('All files use CRLF line endings', fg='green')
        return

    listed = '\n'.join(mismatches)
    if fix:
        click.secho(f'Converted {len(mismatches)} files to CRLF:\n{listed}', fg='yellow')
    else:
        click.secho(f'{len(mismatches)} files have non-CRLF line endings:\n{listed}', fg='red')
        raise SystemExit(1)

if __name__ == '__main__':
    main()
