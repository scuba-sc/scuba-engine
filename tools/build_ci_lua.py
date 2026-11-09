#!/usr/bin/env python3
"""Build the candidate CI's Windows-only Lua CLI from verified upstream source."""
import hashlib
from http.client import IncompleteRead
from io import BytesIO
from pathlib import Path
import subprocess
import sys
import tarfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

# Upstream source and digest: https://www.lua.org/ftp/ (same Lua release as Core).
VERSION = '5.4.8'
URL = f'https://www.lua.org/ftp/lua-{VERSION}.tar.gz'
SHA256 = '4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae'


def download_source():
    """Retry only transient transport failures, never source verification failures."""
    for attempt in range(3):
        try:
            with urlopen(URL, timeout=60) as response:
                return response.read()
        except (HTTPError, URLError, TimeoutError, ConnectionError, IncompleteRead) as error:
            if isinstance(error, HTTPError):
                transient = error.code in (408, 429, 500, 502, 503, 504)
                error.close()
            else:
                reason = error.reason if isinstance(error, URLError) else error
                transient = isinstance(reason, (TimeoutError, ConnectionError, IncompleteRead))
            if not transient or attempt == 2:
                raise
            delay = 2 ** (attempt + 1)
            print(f'Lua download failed ({error}); retrying in {delay}s', file=sys.stderr)
            time.sleep(delay)


def unpack(data, root):
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Lua source SHA-256 mismatch')
    # Copy only flat C/header members, without trusting archive extraction paths.
    prefix = f'lua-{VERSION}/src/'
    with tarfile.open(fileobj=BytesIO(data), mode='r:gz') as archive:
        for member in archive.getmembers():
            if not member.name.startswith(prefix):
                continue
            name = member.name[len(prefix):]
            if '/' in name or '\\' in name or not name.endswith(('.c', '.h')):
                continue
            if not member.isfile():
                raise ValueError('unexpected non-file Lua source member')
            (root / name).write_bytes(archive.extractfile(member).read())
    if not (root / 'lua.c').is_file():
        raise ValueError('Lua CLI source is missing')


def build(root):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=False)  # Never reuse stale source/object/executable bytes.
    unpack(download_source(), root)
    sources = sorted(str(path) for path in root.glob('*.c') if path.name != 'luac.c')
    executable = root / 'lua54.exe'
    subprocess.run(['cl', '/nologo', '/O2', '/W3', '/D_CRT_SECURE_NO_WARNINGS',
                    f'/Fe:{executable}', *sources], cwd=root, check=True)
    subprocess.run([str(executable), '-e', 'assert(_VERSION == "Lua 5.4")'], check=True)
    print(f'Built and executed verified Lua {VERSION}: {executable}')


if __name__ == '__main__':
    if sys.platform != 'win32' or len(sys.argv) != 2:
        raise SystemExit('usage (MSVC x64 developer environment): build_ci_lua.py FRESH_OUTPUT_DIR')
    build(sys.argv[1])
