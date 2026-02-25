import click
import jsoncfg
from jsoncfg.config_classes import ConfigJSONObject, ConfigJSONArray, ConfigJSONScalar
import requests
import tempfile
import hashlib
import tarfile
import configparser
import magic
import os.path
import git
import re
import sys
from github import Github


USR_LIB_WSL = '/usr/lib/wsl'
USR_LIBEXEC_WSL = '/usr/libexec/wsl'
USR_SHARE_WSL = '/usr/share/wsl'

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
                            'NetworkManager-wait-online.service',
                            'networking.service',
                            'hypervkvpd.service']

WSL1_UNSUPPORTED_XATTRS = ['security.selinux', 'security.ima', 'security.evm']

WSL_CONF_KEYS = ['automount.enabled',
                 'automount.ldconfig',
                 'automount.mountfstab',
                 'automount.options',
                 'automount.root',
                 'boot.command',
                 'boot.protectbinfmt',
                 'boot.systemd',
                 'fileserver.enabled',
                 'filesystem.umask',
                 'general.hostname',
                 'gpu.appendlibpath',
                 'gpu.enabled',
                 'interop.appendwindowspath',
                 'interop.enabled',
                 'network.generatehosts',
                 'network.generateresolvconf',
                 'network.hostname',
                 'time.usewindowstimezone',
                 'user.default']

errors = {}
warnings = {}

def subset(inner, outer) -> bool:
    for key, value in inner:
        if key not in outer:
            return True

        if not node_equals(value, outer[key]):
            return False

    return True

def node_equals(left, right):
    if isinstance(left, ConfigJSONScalar):
        return left() == right()

    elif isinstance(left, ConfigJSONArray):
        return len(left) == len(right) and all(node_equals(l, r) for l, r in zip(left, right))
    else:
        return subset(left, right) and subset(right, left)

@click.command()
@click.option('--manifest', default=None)
@click.option('--tar', default=None)
@click.option('--compare-with-branch')
@click.option('--repo-path', '..')
@click.option('--arm64', is_flag=True)
@click.option('--debug', is_flag=True)
def main(manifest: str, tar: str, compare_with_branch: str, repo_path: str, arm64: bool, debug: bool):
    try:
        if tar is not None:
            with open(tar, 'rb') as fd:
                read_tar(None, fd, ARM64_ELF_MAGIC if arm64 else  X64_ELF_MAGIC)
        else:
            if manifest is None:
                raise RuntimeError('Either --tar or --manifest is required')

            manifest_content = jsoncfg.load_config(manifest)

            baseline_manifest = None
            if compare_with_branch is not None:
                repo = git.Repo(repo_path)
                baseline_json = repo.commit(compare_with_branch).tree / 'distributions/DistributionInfo.json'
                baseline_manifest = jsoncfg.loads_config(baseline_json.data_stream.read().decode())['ModernDistributions']

            for flavor, versions in manifest_content["ModernDistributions"]:
                baseline_flavor = baseline_manifest[flavor] if baseline_manifest and flavor in baseline_manifest else None

                for e in versions:
                    name = e['Name']() if 'Name' in e else None

                    if name is None:
                        error(flavor, 'Found nameless distribution')
                        continue

                    if baseline_flavor is not None:
                        baseline_version = next((entry for entry in baseline_flavor if entry['Name']() == name), None)
                        if baseline_version is None:
                            click.secho(f'Found new entry for flavor "{flavor}": {name}', fg='green', bold=True)
                        elif not node_equals(baseline_version, e):
                            click.secho(f'Found changed entry for flavor "{flavor}": {name}', fg='green', bold=True)
                        else:
                            click.secho(f'Distribution entry "{flavor}/{name}" is unchanged, skipping')
                            continue

                    click.secho(f'Reading information for distribution: {name}', bold=True)
                    if 'FriendlyName' not in e:
                        error(e, 'Manifest entry is missing a "FriendlyName" entry')

                    if not name.startswith(flavor):
                        error(e, f'Name should start with "{flavor}"')

                    url_found = False

                    if 'Amd64Url' in e:
                       read_url(e['Amd64Url'], X64_ELF_MAGIC)
                       url_found = True

                    if 'Arm64Url' in e:
                       read_url(e['Arm64Url'], ARM64_ELF_MAGIC)
                       url_found = True

                    if not url_found:
                        error(flavor, 'No URL found')

                    expectedKeys = ['Name', 'FriendlyName', 'Default', 'Amd64Url', 'Arm64Url']
                    for key, value in e:
                        if key not in expectedKeys:
                            error(e, f'Unexpected key: "{key}"')

                default_entries = sum(1 for e in versions if 'Default' in e and e['Default']())
                if default_entries != 1:
                    error(e, f'Found no default distribution for "{flavor}"' if default_entries == 0 else f'Found multiple default distributions for "{flavor}"')

            report_status_on_pr(manifest)

            sys.exit(1 if errors else 0)

    except:
        if debug:
            import traceback
            traceback.print_exc()
            import pdb
            pdb.post_mortem()
        else:
            raise

def report_status_on_pr(manifest: str):
    def format_list(entries: list) -> str:
        if len(entries) == 1:
            return entries[0]

        output = ''
        for e in entries:
            output += f'\n* {e}'

        return output

    for line, text in errors.items():
        escaped = format_list(text).replace('\n', '%0A')
        print(f'::error file={manifest},line={line}::Error: {escaped}')

    for line, text in warnings.items():
        escaped = format_list(text).replace('\n', '%0A')
        print(f'::warning file={manifest},line={line}::Warning: {escaped}')


def read_config_keys(config: configparser.ConfigParser) -> dict:
    keys = {}

    for section in config.sections():
        for key in config[section].keys():
            keys[f'{section}.{key.lower()}'] = config[section][key]

    return keys

def read_passwd(node, default_uid: int, fd):
    def read_passwd_line(line: str):
        fields = line.split(':')

        if len(fields) != 7:
            error(node, f'Invalid passwd entry: {line}')
            return None, None
        try:
            uid = int(fields[2])
        except ValueError:
            error(node, f'Invalid passwd entry: {line}')
            return None, None

        return uid, fields

    entries = {}

    for line in fd.readlines():
        uid, fields = read_passwd_line(line.decode())

        if uid in entries:
            error(node, f'found duplicated uid in /etc/passw: {uid}')
        else:
            entries[uid] = fields

    if 0 not in entries:
        error(node, f'No root (uid=0) found in /etc/passwd')
    elif entries[0][0] != 'root':
        error(node, f'/etc/passwd has a uid=0, but it is not root: {entries[0][0]}')

    if default_uid is not None and default_uid in entries:
        warning(node, f'/etc/passwd already has an entry for default uid: {entries[default_uid]}')

# This logic isn't perfect at listing all boot units, but parsing all of systemd configuration would be too complex.
def read_systemd_enabled_units(node, tar) -> dict:
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

def find_unsupported_attrs(tar):
    found_xattrs = set()
    first_file = None

    for e in tar.getmembers():
        for name in e.pax_headers:
            if any(name.startswith('SCHILY.xattr.' + xattr) for xattr in WSL1_UNSUPPORTED_XATTRS):
                found_xattrs.add(name.replace('SCHILY.xattr.', ''))

                if first_file is None:
                    first_file  = e.name

    return first_file, found_xattrs


def read_tar(node, file, elf_magic: str):
    with tarfile.open(fileobj=file) as tar:

        def validate_mode(path: str, mode, uid, gid, max_size = None, optional = False, follow_symlink = False, magic = None, parse_method = None):
            info, real_path = get_tar_file(tar, path, follow_symlink)

            if info is None:
                if not optional:
                    error(node, f'File "{path}" not found in tar')
                return False

            permissions = oct(info.mode)
            if permissions not in mode:
                warning(node, f'file: "{path}" has unexpected mode: {permissions} (expected: {mode})')

            if info.uid != uid:
                warning(node, f'file: "{path}" has unexpected uid: {info.uid} (expected: {uid})')

            if gid is not None and info.gid != gid:
                warning(node, f'file: "{path}" has unexpected gid: {info.gid} (expected: {gid})')

            if max_size is not None and info.size > max_size:
                error(node, f'file: "{path}" is too big (info.size), max: {max_size}')

            if magic is not None or parse_method is not None:
                content = tar.extractfile(real_path)

                if parse_method is not None:
                    parse_method(content)

                if magic is not None:
                    content.seek(0)
                    buffer = content.read(256)
                    file_magic = MAGIC.from_buffer(buffer)
                    if not magic.match(file_magic):
                        error(node, f'file: "{path}" has unexpected magic type: {file_magic} (expected: {magic})')

            return True

        def validate_config(path: str, valid_keys: list):
            _, path = get_tar_file(tar, path, follow_symlink=True)
            if path is None:
                error(node, f'File "{file}" not found in tar')
                return None

            content = tar.extractfile(path)
            config = configparser.ConfigParser()
            config.read_string(content.read().decode())

            keys = read_config_keys(config)

            unexpected_keys = [e for e in keys if e.casefold() not in valid_keys]
            if unexpected_keys:
                error(node, f'Found unexpected_keys in "{path}": {unexpected_keys}')
            else:
                click.secho(f'Found valid keys in "{path}": {list(keys.keys())}')

            return keys

        defaultUid = None
        if validate_mode('/etc/wsl-distribution.conf', [oct(0o664), oct(0o644)], 0, 0, follow_symlink=True):
            config = validate_config('/etc/wsl-distribution.conf', ['oobe.command', 'oobe.defaultuid', 'shortcut.icon', 'shortcut.enabled', 'oobe.defaultname', 'windowsterminal.profiletemplate', 'windowsterminal.enabled'])

            if oobe_command := config.get('oobe.command', None):
                validate_mode(oobe_command, [oct(0o775), oct(0o755)], 0, 0)

                if not oobe_command.startswith(USR_LIB_WSL) and not oobe_command.startswith(USR_LIBEXEC_WSL):
                    warning(node, f'value for oobe.command is not under {USR_LIB_WSL} or {USR_LIBEXEC_WSL}: "{oobe_command}"')

            if defaultUid := config.get('oobe.defaultuid', None):
                if defaultUid != '1000':
                    warning(node, f'Default UID is not 1000. Found: {defaultUid}')

                defaultUid = int(defaultUid)

            if shortcut_icon := config.get('shortcut.icon', None):
                validate_mode(shortcut_icon, [oct(0o664), oct(0o644)], 0, 0, 1024 * 1024)

                if not shortcut_icon.startswith(USR_LIB_WSL) and not shortcut_icon.startswith(USR_SHARE_WSL):
                    warning(node, f'value for shortcut.icon is not under {USR_LIB_WSL} or {USR_SHARE_WSL}: "{shortcut_icon}"')
            else:
                warning(node, 'No shortcut.icon provided')

            if terminal_profile := config.get('windowsterminal.profileTemplate', None):
                validate_mode(terminal_profile, [oct(0o660), oct(0o640)], 0, 0, 1024 * 1024)

                if not terminal_profile.startswith(USR_LIB_WSL):
                    warning(node, f'value for windowsterminal.profileTemplate is not under {USR_LIB_WSL}: "{terminal_profile}"')

        if validate_mode('/etc/wsl.conf', [oct(0o664), oct(0o644)], 0, 0, optional=True, follow_symlink=True):
            config = validate_config('/etc/wsl.conf', WSL_CONF_KEYS)
            if config.get('boot.systemd', False):
                validate_mode('/sbin/init', [oct(0o775), oct(0o755), oct(0o555)], 0, 0, magic=elf_magic, follow_symlink=True)

            if (default_user := config.get('user.default')) is not None:
                warning(node, f'Found discouraged wsl.conf key: user.default={default_user}')

        validate_mode('/etc/passwd', [oct(0o664), oct(0o644)], 0, 0, parse_method = lambda fd: read_passwd(node, defaultUid, fd))
        validate_mode('/etc/shadow', [oct(0o640), oct(0o600), oct(0)], 0, None)
        validate_mode('/bin/bash', [oct(0o755), oct(0o775), oct(0o555)], 0, 0, magic=elf_magic, follow_symlink=True, optional=True)
        validate_mode('/bin/sh', [oct(0o755), oct(0o775), oct(0o555)], 0, 0, magic=elf_magic, follow_symlink=True)

        enabled_systemd_units = read_systemd_enabled_units(node, tar)
        for unit, path in enabled_systemd_units.items():
            if unit in DISCOURAGED_SYSTEM_UNITS:
                warning(node, f'Found discouraged system unit: {path}')

        first_file, found_xattrs = find_unsupported_attrs(tar)
        if first_file is not None:
            warning(node, f'Found extended attributes that are not supported in WSL1: {found_xattrs}. Sample file: {first_file}')

def read_url(url: dict, elf_magic):
     hash = hashlib.sha256()
     address = url['Url']()

     if not address.endswith('.wsl'):
         warning(url, f'Url does not point to a .wsl file: {address}')

     tar_format = None
     if address.startswith('file://'):
         with open(address.replace('file:///', '').replace('file://', ''), 'rb') as fd:
            while True:
                e = fd.read(4096 * 4096 * 10)
                if not e:
                    break

                hash.update(e)

                if tar_format is None:
                    tar_format = MAGIC.from_buffer(e)

            fd.seek(0, 0)
            read_tar(url, fd, elf_magic)
     else:
         with requests.get(address, stream=True) as response:

            try:
                response.raise_for_status()
            except Exception as e:
                error(url, str(e))
                return

            with tempfile.NamedTemporaryFile() as file:
                for e in response.iter_content(chunk_size=4096 * 4096):
                    file.write(e)
                    hash.update(e)

                    if tar_format is None:
                        tar_format = MAGIC.from_buffer(e)

                file.seek(0, 0)
                read_tar(url, file, elf_magic)


     expected_sha = url['Sha256']() if 'Sha256' in url else None
     if expected_sha is None:
         error(url, 'URL is missing "Sha256"')
     else:
         if expected_sha.startswith('0x'):
             expected_sha = expected_sha[2:]

         sha = hash.digest()
         if bytes.fromhex(expected_sha) != sha:
            error(url, f'URL {address} Sha256 does not match. Expected: {expected_sha}, actual: {hash.hexdigest()}')
         else:
             click.secho(f'Hash for {address} matches ({expected_sha})', fg='green')

     known_format = next((value for key, value in KNOWN_TAR_FORMATS.items() if re.match(key, tar_format)), None)
     if known_format is None:
        error(url, f'Unknown tar format: {tar_format}')
     elif not known_format:
        warning(url, f'Tar format not supported by WSL1: {tar_format}')

def error(node, message: str):
    if node is None:
        click.secho(f'Error: {message}', fg='red')
    else:
        global errors

        line = jsoncfg.node_location(node).line
        click.secho(f'Error on line {line}: {message}', fg='red')

        errors[line] = errors.get(line, []) + [message]

def warning(node, message: str):
    if node is None:
        click.secho(f'Warning: {message}', fg='yellow')
    else:
        global warnings

        line = jsoncfg.node_location(node).line
        click.secho(f'Warning on line {line}: {message}', fg='yellow')

        warnings[line] = warnings.get(line, []) + [message]

if __name__ == "__main__":
    main()
