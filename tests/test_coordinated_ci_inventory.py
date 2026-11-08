#!/usr/bin/env python3
"""Negative controls for candidate CTest inventory gate."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_coordinated_ci_inventory import REQUIRED, WINDOWS_REQUIRED
SCRIPT = ROOT / 'tools/check_coordinated_ci_inventory.py'


class InventoryGuard(unittest.TestCase):
    def test_inventory_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'inventory.json'
            def run(names, platform='linux'):
                path.write_text(json.dumps({'tests': [{'name': n} for n in names]}))
                return subprocess.run([sys.executable, str(SCRIPT), str(path), platform],
                                      capture_output=True, text=True).returncode
            self.assertEqual(run(sorted(REQUIRED)), 0)
            self.assertEqual(run(sorted(REQUIRED | WINDOWS_REQUIRED), 'windows'), 0)
            self.assertNotEqual(run([]), 0)
            self.assertNotEqual(run(sorted(REQUIRED - {'kernel_lifecycle_stop-retained'})), 0)
            self.assertNotEqual(run(sorted(REQUIRED), 'windows'), 0)
            self.assertNotEqual(run(sorted(REQUIRED) + ['kernel_lifecycle_clean']), 0)


if __name__ == '__main__':
    unittest.main()
