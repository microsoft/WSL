import argparse
import os.path
import subprocess

EXTENSIONS = ['.c', '.cpp', '.h', '.hpp', '.idl', '.resw']

def is_source_file(path: str) -> bool:
    return any(path.casefold().endswith(e) for e in EXTENSIONS)

def has_crlf_mismatch(content: bytes) -> bool:
    # Strip all CRLF pairs, then any remaining lone '\n' or '\r' is a mismatch.
    stripped = content.replace(b'\r\n', b'')
    return b'\n' in stripped or b'\r' in stripped

def to_crlf(content: bytes) -> bytes:
    # Normalize every line ending (CRLF, lone CR, lone LF) to a single CRLF.
    return content.replace(b'\r\n', b'\n').replace(b'\r', b'\n').replace(b'\n', b'\r\n')

def main(path: str, fix: bool):
    tracked = subprocess.run(
        ['git', '-C', path, 'ls-files', '-z'], check=True, stdout=subprocess.PIPE).stdout.decode('utf-8').split('\0')
    source_files = [os.path.join(path, e) for e in tracked if e and is_source_file(e)]

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
        print(f'All {len(source_files)} files use CRLF line endings')
        return

    listed = '\n'.join(mismatches)
    if fix:
        print(f'Converted {len(mismatches)} files to CRLF:\n{listed}')
    else:
        print(f'{len(mismatches)} files have non-CRLF line endings:\n{listed}')
        raise SystemExit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Validate that source files use CRLF line endings.')
    parser.add_argument('path', help='Path to validate (must be inside the repo).')
    parser.add_argument('--fix', action='store_true', help='Convert mismatching files to CRLF line endings.')
    args = parser.parse_args()

    main(args.path, args.fix)
