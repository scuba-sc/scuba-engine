#!/usr/bin/env python3
"""Exercise the early local-development input guard when CMake is installed."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

GUARD = Path(__file__).resolve().parents[1] / 'cmake/ScubaDevInputs.cmake'
CMAKE = shutil.which('cmake')


@unittest.skipUnless(CMAKE, 'CMake is required to execute the guard')
class DevelopmentGuard(unittest.TestCase):
    def test_missing_bad_and_checked_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'sco-sdk-host-source-sc-2.4.1-dev.1.zip'
            archive.write_bytes(b'synthetic bytes for guard only')
            sha = hashlib.sha256(archive.read_bytes()).hexdigest()
            base = {'SCUBA_SDK_VERSION': '2.4.1-dev.1',
                    'SCUBA_SDK_ARCHIVE_DIR': tmp,
                    'SCUBA_SDK_HOST_SOURCE_SHA256': sha,
                    'SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT': '1' * 40,
                    'SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256': sha,
                    'SCUBA_CORE_LIFECYCLE_CANDIDATE': 'ON'}
            cases = [({}, True),
                     ({'SCUBA_SDK_ARCHIVE_DIR': ''}, False),
                     ({'SCUBA_SDK_HOST_SOURCE_SHA256': '0' * 64,
                       'SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256': '0' * 64}, False),
                     ({'SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT': ''}, False),
                     ({'SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256': '0' * 64}, False),
                     ({'SCUBA_CORE_LIFECYCLE_CANDIDATE': 'OFF'}, False)]
            for overrides, success in cases:
                with self.subTest(overrides=overrides):
                    values = base | overrides
                    result = subprocess.run([CMAKE, *(f'-D{k}={v}' for k, v in values.items()),
                                             '-P', str(GUARD)], capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
