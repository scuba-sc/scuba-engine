#!/usr/bin/env python3
"""Verify the unpublished public plugin kit, never a host-source or stable kit."""
import hashlib
import json
from pathlib import Path
import stat
import sys
import zipfile

VERSION = '2.4.1-dev.1'
PREFIX = f'sco-sdk-{VERSION}/'


def verify(archive, expected_hash, expected_commit):
    archive = Path(archive)
    if archive.name != PREFIX[:-1] + '.zip':
        raise ValueError('unexpected development plugin archive filename')
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected_hash:
        raise ValueError('development plugin archive SHA-256 mismatch')
    with zipfile.ZipFile(archive) as kit:
        names = kit.namelist()
        if len(names) != len(set(names)):
            raise ValueError('duplicate archive entry')
        for info in kit.infolist():
            name = info.orig_filename
            # ZipInfo normalizes Windows separators and truncates NUL names.
            # Reject that transformation before trusting its lookup filename.
            if name != info.filename:
                raise ValueError('unsafe archive entry normalization')
            if (not name.startswith(PREFIX) or '\\' in name or '\0' in name or ':' in name or
                    any(part in ('', '.', '..') for part in name.split('/')) or
                    stat.S_ISLNK(info.external_attr >> 16)):
                raise ValueError('unsafe archive entry')
            relative = name[len(PREFIX):]
            if relative.startswith(('src/', 'host/', 'internal/')) or '/internal/' in relative:
                raise ValueError('private host content in plugin archive')
        metadata = json.loads(kit.read(PREFIX + 'COMPATIBILITY.json'))
        if (metadata.get('profile') != 'legacy' or
                metadata.get('core_sdk_version') != VERSION or
                metadata.get('plugin_abi_version') != '1.1' or
                metadata.get('source_commits') != {'core': expected_commit, 'game-sc': expected_commit} or
                kit.read(PREFIX + 'VERSION').decode().strip() != VERSION):
            raise ValueError('development plugin source/version/profile mismatch')
        verified = set()
        for line in kit.read(PREFIX + 'SHA256SUMS').decode().splitlines():
            digest, separator, name = line.partition('  ')
            entry = PREFIX + name
            if not separator or entry in verified or entry not in names:
                raise ValueError('invalid checksum inventory')
            if hashlib.sha256(kit.read(entry)).hexdigest() != digest:
                raise ValueError('entry checksum mismatch: ' + name)
            verified.add(entry)
        if verified != set(names) - {PREFIX + 'SHA256SUMS'}:
            raise ValueError('incomplete checksum inventory')
        for required in ('include/sco_api.h', 'cmake/sco-plugin.cmake', 'tools/sco-plugin-check.c'):
            if PREFIX + required not in verified:
                raise ValueError('missing public consumer input: ' + required)
    return len(verified)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: verify_dev_plugin_sdk.py ARCHIVE SHA256 CORE_COMMIT')
    try:
        count = verify(*sys.argv[1:])
    except (ValueError, KeyError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error))
    print(f'verified unpublished plugin SDK {VERSION}, source {sys.argv[3]}, {count} entries')
