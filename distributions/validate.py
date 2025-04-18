import requests
import json
import sys
import hashlib
import base64
import difflib
from urllib.request import urlretrieve
from xml.etree import ElementTree
import tempfile
import zipfile

def download_and_get_manifest(url: str):
    print(f'Downloading {url}')

    filename, _ = urlretrieve(url)
    with zipfile.ZipFile(filename) as archive:
        try:
            with archive.open('AppxManifest.xml') as manifest:
                return ElementTree.fromstring(manifest.read())
        except KeyError:
            # In the case of a bundle
            with archive.open('AppxMetadata/AppxBundleManifest.xml') as manifest:
                return ElementTree.fromstring(manifest.read())

def validate_package_url(url: str, family_name: str, platform: str):
    manifest = download_and_get_manifest(url)
    identity = manifest.find('.//{http://schemas.microsoft.com/appx/manifest/foundation/windows10}Identity')
    dependencies = manifest.find('.//{http://schemas.microsoft.com/appx/manifest/foundation/windows10}PackageDependency')
    if identity is not None:
        # Check the architecture if the package isn't bundled
        assert platform == identity.attrib['ProcessorArchitecture']
    else:
        # Only check the package name for bundles
        identity =  manifest.find('.//{http://schemas.microsoft.com/appx/2013/bundle}Identity')
        dependencies =  manifest.find('.//{http://schemas.microsoft.com/appx/2013/bundle}PackageDependency')

    # Packages uploaded to the CDN shouldn't have dependencies since they can't be installed automatically on Server SKU's.
    assert dependencies is None

    # Validate the package family_name (the last part is based on a custom hash of the publisher)
    publisher_hash = hashlib.sha256(identity.attrib['Publisher'].encode('utf-16le')).digest()[:8]
    encoded_string = ''.join(['{0:b}'.format(e).rjust(8, '0') for e in publisher_hash] + ['0'])
    encoded_hash = ''
    charset = "0123456789abcdefghjkmnpqrstvwxyz"
    for i in range(0, len(encoded_string), 5):
        encoded_hash += charset[int(encoded_string[i:i + 5], 2)]

    assert family_name.startswith(identity.attrib["Name"])
    assert family_name.endswith('_' + encoded_hash)

def validate_distro(distro: dict):
    if distro['Amd64PackageUrl'] is not None:
        validate_package_url(distro['Amd64PackageUrl'], distro['PackageFamilyName'], 'x64')

    if distro['Arm64PackageUrl'] is not None:
        validate_package_url(distro['Arm64PackageUrl'], distro['PackageFamilyName'], 'arm64')

def is_unique(collection: list):
    unique_list = set(collection)
    return len(collection) == len(unique_list)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} /path/to/file [distroName]', file=sys.stderr)
        exit(1)

    with open(sys.argv[1]) as fd:
        data = fd.read()
        content = json.loads(data)
        diff = difflib.unified_diff(
            data.splitlines(keepends=True),
            (json.dumps(content, indent=4) + "\n").splitlines(keepends=True),
            fromfile="a" + sys.argv[1],
            tofile="b" + sys.argv[1],
        )
        diff = "".join(diff)
        assert diff == "", diff

    distros = content['Distributions']
    assert is_unique([e.get('StoreAppId') for e in distros if e])
    assert is_unique([e.get('Name') for e in distros if e])
    
    if len(sys.argv) > 2:
        # Filter the distros to only the one we want to validate
        content = { "Distributions": [e for e in content['Distributions'] if e['Name'] == sys.argv[2]] }
        if not content['Distributions']: 
                raise RuntimeError(f'No distro found for name {sys.argv[2]}')


    for e in content['Distributions']:
        validate_distro(e)

    print("All checks completed successfully")
