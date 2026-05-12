# pip install click

import sys
import os
import re
import xml.etree.ElementTree
from xml.sax.saxutils import escape

def validate_line_endings(path: str, content: bytes):
    line = 0
    for i in range(0, len(content)):
        if content[i] == ord('\n'):
            if i == 0 or content[i - 1] != ord('\r'):
                raise RuntimeError(f'Incorrect line ending (expected CRLF) in {path}:{line}')

            line += 1

def get_strings_from_file(path: str, check_line_endings: bool) -> list:
    with open(path, 'rb') as fd:
        content = fd.read()

    if check_line_endings:
        validate_line_endings(path, content)

    content = xml.etree.ElementTree.fromstring(content.decode())

    result = {}

    for e in content.findall('./data'):
        nodes = list(e.iter())

        text = next(n.text for n in nodes if n.tag == 'value')
        comment = next((n.text for n in nodes if n.tag == 'comment'), '')
        name = e.get('name')

        if name in result:
            raise RuntimeError(f'error: String "{name}" is duplicated in file "{path}"')

        result[name] = text, comment

    return result

def cut_insert(insert: str) -> str:
    index = 1
    while index < len(insert) and insert[index] != '$':
        index += 1

    if index + 1 >= len(insert) or insert[index] != '$':
        raise RuntimeError(f'Invalid insert: {insert}')

    index += 1

    if insert[index] in ['h', 'l']:
        index += 1

    return insert[0:index]

def get_inserts_in_string(string: str) -> int:
    return string.replace('{{', '').count('{')

def get_file_string_inserts(strings: list) -> dict:
    return {name: get_inserts_in_string(value[0]) for name, value in strings.items()}

def validate_resource(baseline: dict, path: str):
    strings = get_strings_from_file(path, False)
    resource = get_file_string_inserts(strings)

    result = True
    for string, inserts in baseline.items():
        if string not in resource:
            print(f'warning: string {string} found in baseline but not in {path}')
            continue

        if inserts != resource[string]:
            print(f'error: Different inserts found for string {string}. Baseline: {inserts}, {path}: {resource[string]}')
            result = False

        comment = strings[string][1]
        locked_strings = re.findall('{Locked="([^}]*)"}', comment, re.DOTALL)
        for e in locked_strings:
            if e not in strings[string][0]:
                print(f'error: locked string "{e}" not found in string {string}: {strings[string][0]}')
                result = False

    return result

def find_argument_end(argument: str) -> int:
    for i in range(len(argument)):
        if not argument[i].isalnum() and argument[i] != '-' and argument[i] != '%':

            # Include one extra character after the argument so that nothing gets added after the argument
            # See: https://github.com/microsoft/WSL/issues/10756
            return min(len(argument), i + 1)

    return len(argument)

def get_locked_strings(name: str, string: str) -> tuple[list, bool]:
    strings = []

    def add_arguments(prefix):
        for i, e in enumerate(string.split(prefix)):
            if i == 0 and not e.startswith(prefix):
                continue

            stop_index = find_argument_end(e)
            if stop_index > 1:
                strings.append(prefix + e[:stop_index])

    add_arguments('--')

    if 'wslconfig'.lower() in name.lower():
        add_arguments('/') # Edge case for wslconfig

    if '.wslconfig'.lower() in string.lower():
        strings.append('.wslconfig')

    return strings, '{}' in string

def generate_string_comment(arguments: list, uses_insert: bool) -> str:
    insert_rule = '{FixedPlaceholder="{}"}' if uses_insert else ''
    return insert_rule + ''.join(f'{{Locked="{e}"}}' for e in arguments) + 'Command line arguments, file names and string inserts should not be translated'

def validate_comments(strings: dict):
    result = True

    comments_changes = {}
    for name, (string, comment) in strings.items():
        arguments, uses_insert = get_locked_strings(name, string)

        if len(arguments) == 0 and not uses_insert:
            continue # No command line arguments or inserts in this string

        # For the sake of simplicity this logic makes the assumption that comments
        # are always in the same order as of the original string
        expected_comment = generate_string_comment(arguments, uses_insert)
        if not expected_comment in comment:
            comments_changes[name] = (comment, expected_comment)
            print(f'Incorrect comment for string {name}. Expected comment: <comment>{expected_comment}</comment>')
            result = False

    return result, comments_changes

def fix_comments(comments: dict, path: str, strings: dict):
    with open(path, 'rb') as fd:
        content = fd.read()

    missed = 0
    for name, (comment, fixed_comment) in comments.items():
        comment = comment.replace('\n', '\r\n')
        matches = content.count(comment.encode())
        if comment and matches == 1:
            content = content.replace(comment.encode(), fixed_comment.encode())
            continue
        elif not comment or matches == 0:
            # Try to add the comment if it doesn't exist at all
            reconstructed_xml = f'''  <data name="{name}" xml:space="preserve">
    <value>{escape(strings[name][0])}</value>
'''
            suffix = '  </data>'
            pattern = (reconstructed_xml + suffix).replace('\n', '\r\n').encode()
            if content.count(pattern) == 1:
                content = content.replace(
                pattern,
                f'{reconstructed_xml}    <comment>{fixed_comment}</comment>\n{suffix}'.replace('\n', '\r\n').encode())

                continue

        click.secho(f"Couldn't find unique match for comment (name={name}): {comment}. It needs to be manually replaced with: {fixed_comment}")
        missed += 1

    with open(path, 'wb') as fd:
        fd.write(content)

    click.secho(f'Updated file: {path}. {missed} comments need manual changes', fg='green' if missed == 0 else 'yellow', bold=True)


ADML_NS = '{http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions}'
ADML_FOLDER = 'intune'
ADML_FILENAME = 'WSL.adml'

def get_adml_strings(path: str) -> dict:
    """Parse an .adml file and return {id: (value, [locked_tokens])}.

    Locked tokens are extracted from XML comments of the form
    `<!-- {Locked="..."}{Locked="..."} -->` placed immediately before a
    `<string>` element. Non-Locked comments are ignored.
    """
    with open(path, 'rb') as fd:
        content = fd.read()

    root = xml.etree.ElementTree.fromstring(content)
    string_table = root.find(f'.//{ADML_NS}stringTable')
    if string_table is None:
        return {}

    strings = {sid: (text, []) for sid, text in
               ((s.get('id'), s.text or '') for s in string_table.findall(f'{ADML_NS}string'))
               if sid is not None}

    text = content.decode('utf-8')
    pattern = re.compile(r'<!--(.*?)-->\s*<string\s+id="([^"]+)"', re.DOTALL)
    for m in pattern.finditer(text):
        sid = m.group(2)
        tokens = re.findall(r'\{Locked="([^"]*)"\}', m.group(1))
        if tokens and sid in strings:
            value, _ = strings[sid]
            strings[sid] = (value, tokens)

    return strings

def validate_adml(adml_folder: str, baseline_language: str) -> bool:
    """Validate ADML files. No-op when the baseline file isn't present."""
    baseline_path = f'{adml_folder}/{baseline_language}/{ADML_FILENAME}'
    if not os.path.isfile(baseline_path):
        return True

    print(f'Validating ADML baseline {baseline_path}')
    baseline = get_adml_strings(baseline_path)
    baseline_ids = set(baseline.keys())

    result = True
    for sid, (value, tokens) in baseline.items():
        for tok in tokens:
            if tok not in value:
                print(f'error: locked token "{tok}" not found in baseline ADML string {sid}: {value}')
                result = False

    if not os.path.isdir(adml_folder):
        return result

    for entry in sorted(os.listdir(adml_folder)):
        locale_path = f'{adml_folder}/{entry}/{ADML_FILENAME}'
        if entry == baseline_language or not os.path.isfile(locale_path):
            continue

        print(f'Validating ADML {locale_path}')
        translated = get_adml_strings(locale_path)
        translated_ids = set(translated.keys())

        missing = baseline_ids - translated_ids
        extra = translated_ids - baseline_ids
        if missing:
            print(f'error: ADML {locale_path} is missing string ids: {sorted(missing)}')
            result = False
        if extra:
            print(f'error: ADML {locale_path} has unexpected string ids: {sorted(extra)}')
            result = False

        for sid in baseline_ids & translated_ids:
            _, tokens = baseline[sid]
            tvalue, _ = translated[sid]
            for tok in tokens:
                if tok not in tvalue:
                    print(f'error: locked token "{tok}" not found in {locale_path} string {sid}: {tvalue}')
                    result = False

    return result

def run(resource_folder: str, baseline_language: str, fix: bool, adml_folder: str):
    baseline_file = f'{resource_folder}/{baseline_language}/Resources.resw'

    strings = get_strings_from_file(baseline_file, True)
    baseline = get_file_string_inserts(strings)

    result, comments = validate_comments(strings)
    for language in os.listdir(resource_folder):
        path = f'{resource_folder}/{language}/Resources.resw'
        print(f'Validating inserts in {path}')
        result &= validate_resource(baseline, path)

    result &= validate_adml(adml_folder, baseline_language)

    if fix and comments:
        fix_comments(comments, baseline_file, strings)

    sys.exit(0 if result else 1)


if __name__ == '__main__':
    if len(sys.argv) in (3, 4): # Hack to work around pip install errors in the build pipeline
        adml = sys.argv[3] if len(sys.argv) == 4 else ADML_FOLDER
        run(sys.argv[1], sys.argv[2], False, adml)
    else:
        import click

        @click.command()
        @click.argument('resource-folder', default='localization/strings')
        @click.argument('baseline-language', default='en-us')
        @click.argument('adml-folder', default=ADML_FOLDER)
        @click.option('--fix', is_flag=True)
        def main(resource_folder: str, baseline_language: str, adml_folder: str, fix: bool):
            run(resource_folder, baseline_language, fix, adml_folder)

        main()