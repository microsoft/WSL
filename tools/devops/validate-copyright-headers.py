import glob
import sys
import re
import os.path

EXPECTED_HEADER = '.*Copyright \\(c\\) Microsoft.*All rights reserved.*'.casefold()
EXTENSIONS = ['.c', '.cpp', '.cxx', '.h', '.hpp', '.hxx']

def has_header(path: str) -> bool:
    with open(path, 'rb') as fd:
        lines = fd.read().decode('utf-8', 'ignore').replace('\r', '').split('\n')

    in_multiline_comment = False

    for e in lines[:50]:
        if e.startswith('/*'): # Simplified comment parsing
            in_multiline_comment = True

        if '*/' in e:
            in_multiline_comment = False

        if e.strip().startswith('//') or in_multiline_comment:
            if re.match(EXPECTED_HEADER, e.casefold()):
                return True


    return False

def is_source_file(path: str) -> bool:
    return any(e for e in EXTENSIONS if path.casefold().endswith(e))

def generate_header(path: str):
    with open(path, 'rb') as fd:
        content = fd.read().decode('utf-8', 'ignore')

    header = f'''/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    {os.path.basename(path)}

Abstract:

    TODO

--*/
'''.replace('\n', '\r\n')

    with open(path, 'wb') as fd:
        fd.write((header + content).encode('utf-8'))


def main(path: str, fix: bool):
    files = glob.glob(f'{path}/**', recursive=True)

    source_files = [e for e in files if is_source_file(e)]
    print(f'Validate copyright headers for {len(source_files)} files')

    missing_headers = [e for e in source_files if not has_header(e)]

    if missing_headers:
        if fix:
            for e in missing_headers:
                generate_header(e)

        files = "\n".join(missing_headers)
        print(f'{len(missing_headers)} files are missing a copyright header:\n{files}')
        sys.exit(1)


if __name__ == '__main__':
    path = '.'
    fix = False

    for e in sys.argv[1:]:
        if e == '--fix':
            fix = True
        else:
            path = e

    main(path, fix)