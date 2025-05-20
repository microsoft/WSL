# pip install click
# python gh-release-server.py  2.0.0 --msi ..\..\bin\x64\Debug\wsl.msi --msix ..\..\bin\x64\Debug\installer.msix

import click
from http.server import SimpleHTTPRequestHandler, HTTPServer
from http import HTTPStatus
import socketserver
import json
from winreg import *

RELEASES_PATH = '/releases'
MSI_PATH = '/msi'
MSIX_PATH = '/msix'

@click.command()
@click.argument('version', required=True)
@click.option('--msi', default=None)
@click.option('--port', default=8000)
@click.option('--msix', default=None)
@click.option('--pre-release', default=False, is_flag=True)
def main(port: int, version: str, msi: str, msix: str, pre_release: bool):
    lxss_key = OpenKeyEx(HKEY_LOCAL_MACHINE, '''SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss''', 0, KEY_SET_VALUE)

    assets = []
    if msix:
        assets.append({'url': f'http://127.0.0.1:{port}{MSIX_PATH}', 'id': 0, 'name': 'wsl.msixbundle'})

    if msi:
        assets.append({'url': f'http://127.0.0.1:{port}{MSI_PATH}', 'id': 0, 'name': 'wsl.x64.msi'})

    release_json = {'name': version, 'created_at': '2023-06-14T16:56:30Z', 'assets': assets}
    release_response = json.dumps([release_json] if pre_release else release_json).encode()

    class ReleaseRequestHandler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            return path

        def do_GET(self):
            print(self.path)

            if self.path == RELEASES_PATH:
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-type", 'application/octet-stream')
                self.send_header("Content-Length", str(len(release_response)))
                self.end_headers()

                self.wfile.write(release_response)
            elif self.path == MSI_PATH:
                self.path = msi
                file = self.send_head()
                if file is None:
                    raise RuntimeError(f'Failed to open {msi}')

                self.copyfile(file, self.wfile)
            elif self.path == MSIX_PATH:
                self.path = msix
                file = self.send_head()
                if file is None:
                    raise RuntimeError(f'Failed to open {msix}')

                self.copyfile(file, self.wfile)
            else:
                print(f'Received unexpected request: {self.path}')

    SetValueEx(lxss_key, 'GitHubUrlOverride', 0, REG_SZ, f'http://127.0.0.1:{port}{RELEASES_PATH}')

    try:

        with socketserver.TCPServer(("127.0.0.1", port), ReleaseRequestHandler) as server:
            print(f'Serving on port {port}')
            server.serve_forever()
    finally:
        DeleteValue(lxss_key, 'GitHubUrlOverride')

if __name__ == '__main__':
    main()