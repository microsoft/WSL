import click
import json
import requests
import tempfile
import hashlib
import tarfile
import configparser
import magic
import os.path
import git
import re
from github import Github


USR_LIB_WSL = '/usr/lib/wsl'

MAGIC = magic.Magic()
X64_ELF_MAGIC = re.compile('^ELF 64-bit.* x86-64, version 1')
ARM64_ELF_MAGIC = re.compile('^ELF 64-bit.* ARM aarch64, version 1')

KNOWN_TAR_FORMATS = {'^XZ compressed data.*': True, '^gzip compressed data.*': True}

DISCOURAGED_SYSTEM_UNITS = ['systemd-resolved.service',
                            'systemd-networkd.service',
                            'systemd-networkd-wait-online.service',
                            'systemd-tmpfiles-setup.service',
                            'systemd-tmpfiles-clean.service',
                            'systemd-tmpfiles-setup-dev-early.service',
                            'systemd-tmpfiles-setup-dev.service',
                            'tmp.mount',
                            'NetworkManager.service',
                            'networking.service']

errors = []
warnings = []


@click.command()
@click.option('--manifest', default=None)
@click.option('--tar', default=None)
@click.option('--compare-with-branch')
@click.option('--repo-path', '..')
@click.option('--arm64', is_flag=True)
@click.option('--github-token', default=None)
@click.option('--github-pr', default=None, type=int)
@click.option('--github-commit', default=None)
@click.option('--debug', is_flag=True)
def main(manifest: str, tar: str, compare_with_branch: str, repo_path: str, arm64: bool, github_token: str, github_pr: str, github_commit: str, debug: bool):
    try:
        if tar is not None:
            with open(tar, 'rb') as fd:
                read_tar(tar, '<none>', fd, ARM64_ELF_MAGIC if arm64 else  X64_ELF_MAGIC)
        else:
            if manifest is None:
                raise RuntimeError('Either --tar or --manifest is required')

            with open(manifest) as fd:
                manifest_content = json.loads(fd.read())

            baseline_manifest = None
            if compare_with_branch is not None:
                repo = git.Repo(repo_path)
                baseline_json = repo.commit(compare_with_branch).tree / 'distributions/DistributionInfo.json'
                baseline_manifest = json.load(baseline_json.data_stream).get('ModernDistributions', {})

            for flavor, versions in manifest_content["ModernDistributions"].items():
                baseline_flavor = baseline_manifest.get(flavor, None) if baseline_manifest else None

                for e in versions:
                    name = e.get('Name', None)

                    if name is None:
                        error(flavor, None, 'Found nameless distribution')
                        continue

                    if baseline_flavor is not None:
                        baseline_version = next((entry for entry in baseline_flavor if entry['Name'] == name), None)
                        if baseline_version is None:
                            click.secho(f'Found new entry for flavor "{flavor}": {name}', fg='green', bold=True)
                        elif baseline_version != e:
                            click.secho(f'Found changed entry for flavor "{flavor}": {name}', fg='green', bold=True)
                        else:
                            click.secho(f'Distribution entry "{flavor}/{name}" is unchanged, skipping')
                            continue

                    click.secho(f'Reading information for distribution: {e["Name"]}', bold=True)
                    if 'FriendlyName' not in e:
                        error(flavor, name, 'Manifest entry is missing a "FriendlyName" entry')

                    if not name.startswith(flavor):
                        error(flavor, name, f'Name should start with "{flavor}"')

                    url_found = False

                    if 'Amd64Url' in e:
                       read_url(flavor, name, e['Amd64Url'], X64_ELF_MAGIC)
                       url_found = True

                    if 'Arm64Url' in e:
                       read_url(flavor, name, e['Arm64Url'], ARM64_ELF_MAGIC)
                       url_found = True

                    if not url_found:
                        error(flavor, name, 'No URL found')

                    expectedKeys = ['Name', 'FriendlyName', 'Default', 'Amd64Url', 'Arm64Url']
                    for key in e.keys():
                        if key not in expectedKeys:
                            error(flavor, name, 'Unexpected key: "{key}"')


                default_entries = sum(1 for e in versions if e.get('Default', False))
                if default_entries != 1:
                    error(flavor, None, 'Found no default distribution' if default_entries == 0 else 'Found multiple default distributions')

        if github_pr is not None:
            assert github_token is not None and github_commit is not None and manifest is not None

            report_status_on_pr(github_pr, github_token, github_commit, manifest)

    except:
        if debug:
            import traceback
            traceback.print_exc()
            import pdb
            pdb.post_mortem()
        else:
            raise

def report_status_on_pr(pr: int, github_token: str, github_commit: str, manifest: str):
    github = Github(github_token)
    repo = github.get_repo('microsoft/WSL')

    def format_list(entries: list) -> str:
        output = '\n'

        for e in entries:
            output += f'\n* {e}'

        return output + '\n'

    body = 'Thank you for your contribution to WSL.\n'
    if errors:
        body += f'**The following fatal errors have been found in this pull request:** {format_list(errors)}\n'
    else:
        body += 'No fatal errors have been found.\n'

    if warnings:
        body += f'**The following suggestions have been found in this pull request:** {format_list(warnings)}\n'
    else:
        body += 'No suggestions have been found.\n'

    repo.get_pull(pr).create_review(body=body, commit=repo.get_commit(github_commit))


def read_config_keys(config: configparser.ConfigParser) -> dict:
    keys = {}

    for section in config.sections():
        for key in config[section].keys():
            keys[f'{section}.{key.lower()}'] = config[section][key]

    return keys

def read_passwd(flavor: str, name: str, default_uid: int, fd):
    def read_passwd_line(line: str):
        fields = line.split(':')

        if len(fields) != 7:
            error(flavor, name, f'Invalid passwd entry: {line}')
            return None, None
        try:
            uid = int(fields[2])
        except ValueError:
            error(flavor, name, f'Invalid passwd entry: {line}')
            return None, None

        return uid, fields

    entries = {}

    for line in fd.readlines():
        uid, fields = read_passwd_line(line.decode())

        if uid in entries:
            error(flavor, name, f'found duplicated uid in /etc/passw: {uid}')
        else:
            entries[uid] = fields

    if 0 not in entries:
        error(flavor, name, f'No root (uid=0) found in /etc/passwd')
    elif entries[0][0] != 'root':
        error(flavor, name, f'/etc/passwd has a uid=0, but it is not root: {entries[0][0]}')

    if default_uid is not None and default_uid in entries:
        warning(flavor, name, f'/etc/passwd already has an entry for default uid: {entries[default_uid]}')

# This logic isn't perfect at listing all boot units, but parsing all of systemd configuration would be too complex.
def read_systemd_enabled_units(flavor: str, name: str, tar) -> dict:
    config_dirs = ['/usr/local/lib/systemd/system', '/usr/lib/systemd/system', '/etc/systemd/system']

    all_files = tar.getnames()

    def link_target(unit_path: str):
        try:
            info = tar.getmember(unit_path)
        except KeyError:
            info = tar.getmember('.' + unit_path)

        if not info.issym():
            return unit_path
        else:
            if info.linkpath.startswith('/'):
                return get_tar_file(tar, linux_real_path(info.linkpath), follow_symlink=True)[1]
            else:
                return get_tar_file(tar, linux_real_path(os.path.dirname(unit_path) + '/' + info.linkpath), follow_symlink=True)[1]

    def list_directory(path: str):
        files = []
        for e in all_files:
            if e.startswith(path):
                files.append(e[len(path) + 1:])
            elif e.startswith('.' + path):
                files.append(e[len(path) + 2:])

        return files

    def is_dev_null(path: str) -> bool:
        return path == './dev/null' or path == '/dev/null'

    def is_masked(unit: str):
        try:
            target = link_target(f'/etc/systemd/system/{unit}')
        except KeyError:
            return False # No symlink found, unit is not masked

        return is_dev_null(target)

    units = {}
    for config_dir in config_dirs:
        targets = [e for e in list_directory(config_dir) if e.endswith('.target.wants')]

        for target in targets:
            for e in list_directory(f'{config_dir}/{target}'):
                fullpath = f'{config_dir}/{target}/{e}'

                unit_target = link_target(fullpath)

                if is_dev_null(unit_target) and not is_masked(e):
                    units[e] = fullpath

    return units

# Manually implemented because os.path.realpath tries to resolve local symlinks
def linux_real_path(path: str):
    components = path.split('/')

    result = []
    for e in components:
        if e == '.' or not e:
            continue
        elif e == '..':
            if result:
                del result[-1]
            continue

        result.append(e)

    real_path = '/'.join(result)
    if path and path[0] == '/':
        return '/' + real_path
    else:
        return real_path

def get_tar_file(tar, path: str, follow_symlink=False, symlink_depth=10):
    if symlink_depth < 0:
        print(f'Warning: Exceeded maximum symlink depth when reading: {path}')
        return None, None

    # Tar members can be formatted as /{path}, {path}, or ./{path}
    if path.startswith('/'):
        paths = [path, '.' + path, path[1:]]
    elif path.startswith('./'):
        paths = [path, path[1:], path[2:]]
    else:
        paths = [path, './' + path, '/' + path]

    def follow_if_symlink(info, path: str):
        if follow_symlink and info.issym():
            if info.linkpath.startswith('/'):
                return get_tar_file(tar, info.linkpath, follow_symlink=True, symlink_depth=symlink_depth - 1)
            else:
                return get_tar_file(tar, linux_real_path(os.path.dirname(path) + '/' + info.linkpath), follow_symlink=True, symlink_depth=symlink_depth -1)
        else:
            return info, path

    # First try accessing the file directly
    for e in paths:
        try:
            return follow_if_symlink(tar.getmember(e), e)
        except KeyError:
            continue

    if not follow_symlink:
        return None, None

    # Then look for symlinks
    # The path might be covered by a symlink, check if parent exists and is a symlink
    parent_path = os.path.dirname(path)
    if parent_path != path:
        try:
            parent_info, real_parent_path = get_tar_file(tar, parent_path, follow_symlink=True, symlink_depth=symlink_depth - 1)
            if real_parent_path is not None and real_parent_path != parent_path:
                return get_tar_file(tar, f'{real_parent_path}/{os.path.basename(path)}', follow_symlink=True, symlink_depth=symlink_depth -1)
        except KeyError:
            pass

    return None, None

def read_tar(flavor: str, name: str, file, elf_magic: str):
    with tarfile.open(fileobj=file) as tar:

        def validate_mode(path: str, mode, uid, gid, max_size = None, optional = False, follow_symlink = False, magic = None, parse_method = None):
            info, real_path = get_tar_file(tar, path, follow_symlink)

            if info is None:
                if not optional:
                    error(flavor, name, f'File "{path}" not found in tar')
                return False

            permissions = oct(info.mode)
            if permissions not in mode:
                warning(flavor, name, f'file: "{path}" has unexpected mode: {permissions} (expected: {mode})')

            if info.uid != uid:
                warning(flavor, name, f'file: "{path}" has unexpected uid: {info.uid} (expected: {uid})')

            if gid is not None and info.gid != gid:
                warning(flavor, name, f'file: "{path}" has unexpected gid: {info.gid} (expected: {gid})')

            if max_size is not None and info.size > max_size:
                error(flavor, name, f'file: "{path}" is too big (info.size), max: {max_size}')

            if magic is not None or parse_method is not None:
                content = tar.extractfile(real_path)

                if parse_method is not None:
                    parse_method(content)

                if magic is not None:
                    content.seek(0)
                    buffer = content.read(256)
                    file_magic = MAGIC.from_buffer(buffer)
                    if not magic.match(file_magic):
                        error(flavor, name, f'file: "{path}" has unexpected magic type: {file_magic} (expected: {magic})')

            return True

        def validate_config(path: str, valid_keys: list):
            _, path = get_tar_file(tar, path, follow_symlink=True)
            if path is None:
                error(flavor, name, f'File "{file}" not found in tar')
                return None

            content = tar.extractfile(path)
            config = configparser.ConfigParser()
            config.read_string(content.read().decode())

            keys = read_config_keys(config)

            unexpected_keys = [e for e in keys if e.lower() not in valid_keys]
            if unexpected_keys:
                error(flavor, name, f'Found unexpected_keys in "{path}": {unexpected_keys}')
            else:
                click.secho(f'Found valid keys in "{path}": {list(keys.keys())}')

            return keys

        defaultUid = None
        if validate_mode('/etc/wsl-distribution.conf', [oct(0o664), oct(0o644)], 0, 0):
            config = validate_config('/etc/wsl-distribution.conf', ['oobe.command', 'oobe.defaultuid', 'shortcut.icon', 'oobe.defaultname', 'windowsterminal.profiletemplate'])

            if oobe_command := config.get('oobe.command', None):
                validate_mode(oobe_command, [oct(0o775), oct(0o755)], 0, 0)

                if not oobe_command.startswith(USR_LIB_WSL):
                    warning(flavor, name, f'value for oobe.command is not under {USR_LIB_WSL}: "{oobe_command}"')

            if defaultUid := config.get('oobe.defaultuid', None):
                if defaultUid != '1000':
                    warning(flavor, name, f'Default UID is not 1000. Found: {defaultUid}')

                defaultUid = int(defaultUid)

            if shortcut_icon := config.get('shortcut.icon', None):
                validate_mode(shortcut_icon, [oct(0o664), oct(0o644)], 0, 0, 1024 * 1024)

                if not shortcut_icon.startswith(USR_LIB_WSL):
                    warning(flavor, name, f'value for shortcut.icon is not under {USR_LIB_WSL}: "{shortcut_icon}"')

            if terminal_profile := config.get('windowsterminal.profileTemplate', None):
                validate_mode(terminal_profile, [oct(0o660), oct(0o640)], 0, 0, 1024 * 1024)

                if not terminal_profile.startswith(USR_LIB_WSL):
                    warning(flavor, name, f'value for windowsterminal.profileTemplate is not under {USR_LIB_WSL}: "{terminal_profile}"')

        if validate_mode('/etc/wsl.conf', [oct(0o664), oct(0o644)], 0, 0, optional=True):
            config = validate_config('/etc/wsl.conf', ['boot.systemd'])
            if config.get('boot.systemd', False):
                validate_mode('/sbin/init', [oct(0o775), oct(0o755)], 0, 0, magic=elf_magic, follow_symlink=True)

        validate_mode('/etc/passwd', [oct(0o664), oct(0o644)], 0, 0, parse_method = lambda fd: read_passwd(flavor, name, defaultUid, fd))
        validate_mode('/etc/shadow', [oct(0o640), oct(0o600)], 0, None)
        validate_mode('/bin/bash', [oct(0o755), oct(0o775)], 0, 0, magic=elf_magic, follow_symlink=True)
        validate_mode('/bin/sh', [oct(0o755), oct(0o775)], 0, 0, magic=elf_magic, follow_symlink=True)

        enabled_systemd_units = read_systemd_enabled_units(flavor, name, tar)
        for unit, path in enabled_systemd_units.items():
            if unit in DISCOURAGED_SYSTEM_UNITS:
                warning(flavor, name, f'Found discouraged system unit: {path}')

def read_url(flavor: str, name: str, url: dict, elf_magic):
     hash = hashlib.sha256()
     if not url['Url'].endswith('.wsl'):
         warning(flavor, name, f'Url does not point to a .wsl file: {url["Url"]}')

     tar_format = None
     if url['Url'].startswith('file://'):
         with open(url['Url'].replace('file:///', '').replace('file://', ''), 'rb') as fd:
            while True:
                e = fd.read(4096 * 4096 * 10)
                if not e:
                    break

                hash.update(e)

                if tar_format is None:
                    tar_format = MAGIC.from_buffer(e)

            fd.seek(0, 0)
            read_tar(flavor, name, fd, elf_magic)
     else:
         with requests.get(url['Url'], stream=True) as response:
            response.raise_for_status()

            with tempfile.NamedTemporaryFile() as file:
                for e in response.iter_content(chunk_size=4096 * 4096):
                    file.write(e)
                    hash.update(e)

                    if tar_format is None:
                        tar_format = MAGIC.from_buffer(e)

                file.seek(0, 0)
                read_tar(flavor, name, file, elf_magic)


     expected_sha = url.get('Sha256', None)
     if expected_sha is None:
         error(flavor, name, 'URL is missing "Sha256"')
     else:
         if expected_sha.startswith('0x'):
             expected_sha = expected_sha[2:]

         sha = hash.digest()
         if bytes.fromhex(expected_sha) != sha:
            error(flavor, name, f'URL {url["Url"]} Sha256 does not match. Expected: {expected_sha}, actual: {hash.hexdigest()}')
         else:
             click.secho(f'Hash for {url["Url"]} matches ({expected_sha})', fg='green')

     known_format = next((value for key, value in KNOWN_TAR_FORMATS.items() if re.match(key, tar_format)), None)
     if known_format is None:
        error(flavor, name, f'Unknown tar format: {tar_format}')
     elif not known_format:
        warning(flavor, name, f'Tar format not supported by WSL1: {tar_format}')

def error(flavor: str, distribution: str, message: str):
    global errors

    message = f'{flavor}/{distribution}: {message}'
    click.secho(f'Error: {message}', fg='red')

    errors.append(message)

def warning(flavor: str, distribution: str, message: str):
    global warnings

    message = f'{flavor}/{distribution}: {message}'
    click.secho(f'Warning: {message}', fg='yellow')

    warnings.append(message)
if __name__ == "__main__":
    main()