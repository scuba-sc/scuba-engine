#!/usr/bin/env python3
"""Exercise historical and development host-kit roots and provenance refusals."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERIFIERS = (ROOT / 'tools/verify_dev_sdk.py', ROOT / 'tools/curated_mock_host/verify_archive.py')
COMMIT = '1' * 40


def make_archive(directory, version, *, wrong_root=False, source=COMMIT, unsafe=False, metadata_version=None):
    name = f'sco-sdk-host-source-sc-{version}'
    path = directory / f'{name}.zip'
    top = 'sco-sdk-host-source-sc-wrong' if wrong_root else name
    files = {'COMPATIBILITY.json': json.dumps({'profile': 'host-source-sc',
              'core_sdk_version': metadata_version or version,
              'source_commits': {'core': source, 'game-sc': source}}).encode(),
             'include/example.h': b'example\n'}
    if unsafe:
        files['../escape'] = b'no\n'
    sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {file}\n'
                   for file, data in files.items()).encode()
    with zipfile.ZipFile(path, 'w') as archive:
        for file, data in files.items():
            archive.writestr(f'{top}/{file}', data)
        archive.writestr(f'{top}/SHA256SUMS', sums)
    return path


class HostArchiveVerifiers(unittest.TestCase):
    def run_verifiers(self, archive, expected_commit, *, expected_success, wrong_hash=False):
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if wrong_hash:
            digest = ("0" if digest[0] != "0" else "1") + digest[1:]
        for verifier in VERIFIERS:
            with self.subTest(verifier=verifier.name, archive=archive.name):
                result = subprocess.run([sys.executable, str(verifier), str(archive),
                                         digest, expected_commit], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, expected_success, result.stdout + result.stderr)

    def test_stable_and_development_versions(self):
        with tempfile.TemporaryDirectory() as tmp:
            for version in ('2.4.0', '2.4.1-dev.1'):
                self.run_verifiers(make_archive(Path(tmp), version), COMMIT, expected_success=True)

    def test_wrong_hash_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_verifiers(make_archive(Path(tmp), '2.4.1-dev.1'), COMMIT,
                               expected_success=False, wrong_hash=True)

    def test_wrong_root_source_and_unsafe_path_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for kwargs, expected_source in (({'wrong_root': True}, COMMIT),
                                             ({'source': '2' * 40}, COMMIT),
                                             ({'metadata_version': '2.4.0'}, COMMIT),
                                             ({'unsafe': True}, COMMIT)):
                self.run_verifiers(make_archive(directory, '2.4.1-dev.1', **kwargs),
                                   expected_source, expected_success=False)


if __name__ == '__main__':
    unittest.main()
