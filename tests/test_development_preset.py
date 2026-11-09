#!/usr/bin/env python3
"""Guard the development preset's explicit local inputs and checked lifecycle."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DevelopmentPreset(unittest.TestCase):
    def test_local_and_checked_coordinates(self):
        presets = json.loads((ROOT / 'CMakePresets.json').read_text())
        preset = next(p for p in presets['configurePresets'] if p['name'] == 'coordinated-core-development')
        values = preset['cacheVariables']
        self.assertEqual(values['SCUBA_SDK_VERSION'], '2.4.1-dev.1')
        self.assertEqual(values['SCUBA_SDK_RELEASE_TAG'], '')
        self.assertEqual(values['SCUBA_SDK_ARCHIVE_DIR'], '$env{SCUBA_DEV_SDK_ARCHIVE_DIR}')
        self.assertEqual(values['SCUBA_SDK_HOST_SOURCE_SHA256'], '$env{SCUBA_DEV_SDK_HOST_SHA256}')
        self.assertEqual(values['SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256'], values['SCUBA_SDK_HOST_SOURCE_SHA256'])
        self.assertEqual(values['SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT'], 'd06f9124d6d7d380295ada4bc2af8968b98a051c')
        self.assertEqual(values['SCUBA_CORE_LIFECYCLE_CANDIDATE'], 'ON')
        self.assertEqual(values['SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL'], 'OFF')


if __name__ == '__main__':
    unittest.main()
