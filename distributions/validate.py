import requests
import json
import sys
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

    assert identity.attrib['Name'] in family_name

    # Packages uploaded to the CDN shouldn't have dependencies since they can't be installed automatically on Server SKU's.
    assert dependencies is None

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
        print(f'Usage: {sys.argv[0]} /path/to/file', file=sys.stderr)
        exit(1)

    with open(sys.argv[1]) as fd:
        content = json.loads(fd.read())

    distros = content['Distributions']
    assert is_unique([e.get('StoreAppId') for e in distros if e])
    assert is_unique([e.get('Name') for e in distros if e])

    for e in content['Distributions']:
        validate_distro(e)

    print("All checks completed successfully")