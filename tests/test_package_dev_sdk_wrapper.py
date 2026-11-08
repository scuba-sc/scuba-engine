#!/usr/bin/env python3
"""Ensure the wrapper preserves historical profile selection and forwards development mode."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WRAPPER = Path(__file__).resolve().parents[1] / 'tools/package_dev_sdk.py'


class PackageDevSdkWrapper(unittest.TestCase):
    def test_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'core'
            (root / 'sdk').mkdir(parents=True)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'sdk/package.py').write_text('import json, sys\nprint(json.dumps(sys.argv[1:]))\n')
            output = Path(tmp) / 'out'
            for extra, expected in (([], ['--profile', 'host-source-sc', '--out', str(output)]),
                                    (['legacy'], ['--profile', 'legacy', '--out', str(output)]),
                                    (['--development'], ['--profile', 'host-source-sc', '--out', str(output), '--development'])):
                with self.subTest(extra=extra):
                    result = subprocess.run([sys.executable, str(WRAPPER), str(root), str(output), *extra],
                                            check=True, capture_output=True, text=True)
                    self.assertEqual(json.loads(result.stdout), expected)


if __name__ == '__main__':
    unittest.main()
