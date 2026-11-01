"""Synthetic classifier and no-network process tests, not runtime UDP evidence."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/compare_udp_bounds.py'
spec = importlib.util.spec_from_file_location('compare_udp_bounds', SCRIPT)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

SLOTS = ''.join(f'slot={i} reliable_received=224 unreliable_received=224 unreliable_loss=0\n'
                for i in range(1, 9))
LOSS = 'peer condition timeout: ' + json.dumps({
    'active': True, 'members': 8, 'probe': {'reliableSent': 32, 'unreliableSent': 32,
                                             'reliableReceived': 224, 'unreliableReceived': 122}}) + '\n'
COMPAT = {'schema_version': 1, 'profile': 'host-source-sc', 'base_core_sha256': None, 'core_sdk_version': '2.4.0',
          'game_overlay_version': '0.8.0', 'plugin_abi_version': '1.1',
          'source_commits': {'core': 'a' * 40, 'game-sc': 'b' * 40}}


class ClassifierTests(unittest.TestCase):
    def test_synthetic_loss_and_complete_candidate(self):
        self.assertEqual(harness.classify('baseline', 1, False, LOSS)[0], 'loss')
        self.assertEqual(harness.classify('candidate', 0, False, SLOTS)[0], 'pass')

    def test_fail_closed(self):
        for label, code, timeout, log in [
            ('baseline', -11, False, LOSS), ('baseline', 139, False, LOSS),
            ('candidate', 139, False, SLOTS + 'CHECK failed: membership'),
            ('baseline', 0, False, LOSS), ('baseline', 1, False, LOSS + 'CHECK failed: drop count'),
            ('baseline', 1, False, 'kernel start failed'),
            ('baseline', 1, False, 'sandbox data root already exists'),
            ('candidate', 1, False, 'peer slot is not running'),
            ('baseline', 1, False, 'peer condition timeout: {broken'),
            ('baseline', 1, False, 'peer condition timeout: ' + json.dumps({'probe': {}})),
            ('baseline', 1, True, LOSS), ('candidate', 0, False, ''),
            ('candidate', 0, False, SLOTS + SLOTS), ('candidate', 0, False, SLOTS.replace('slot=8', 'slot=7')),
            ('candidate', 0, False, SLOTS.replace('unreliable_loss=0', 'unreliable_loss=1', 1)),
            ('candidate', 1, False, 'probe send refused: 1')]:
            with self.subTest(label=label, log=log):
                self.assertEqual(harness.classify(label, code, timeout, log)[0], 'inconclusive')
        self.assertEqual(harness.classify('candidate', 1, False, 'assertion failed')[0], 'failure')
        self.assertEqual(harness.classify('candidate', 0, False, SLOTS + 'CHECK failed: membership')[0], 'failure')


class MetadataTests(unittest.TestCase):
    def test_identical_host_binary_is_rejected(self):
        inputs = {name: {'sdk_compatibility': COMPAT, 'test_binary': {'sha256': 'same-test'},
                         'hostd': {'sha256': 'same-host'}}
                  for name in ('baseline', 'candidate')}
        with self.assertRaisesRegex(ValueError, 'hostd binaries have identical SHA-256'):
            harness.validate_inputs(inputs)

    def test_valid_source_coordinates_can_differ(self):
        candidate = dict(COMPAT, source_commits={'core': 'c' * 40, 'game-sc': 'd' * 40})
        inputs = {name: {'sdk_compatibility': compat, 'test_binary': {'sha256': 'same'},
                         'hostd': {'sha256': name}}
                  for name, compat in [('baseline', COMPAT), ('candidate', candidate)]}
        harness.validate_inputs(inputs)

    def test_matching_malformed_metadata_is_rejected(self):
        changes = [
            {'core_sdk_version': 'garbage'}, {'core_sdk_version': '2.4'},
            {'game_overlay_version': '0.8.0-beta'}, {'plugin_abi_version': '1.1.0'},
            {'source_commits': {'core': 'A' * 40, 'game-sc': 'b' * 40}},
            {'source_commits': {'core': 'a' * 64, 'game-sc': 'b' * 40}},
            {'source_commits': {'core': 'a' * 40, 'game-sc': 'b' * 40, 'extra': 'c' * 40}},
            {'base_core_sha256': 'a' * 64},
        ]
        for change in changes:
            compat = dict(COMPAT, **change)
            inputs = {name: {'sdk_compatibility': compat, 'test_binary': {'sha256': 'same'},
                             'hostd': {'sha256': name}}
                      for name in ('baseline', 'candidate')}
            with self.subTest(change=change), self.assertRaises(ValueError):
                harness.validate_inputs(inputs)


@unittest.skipUnless(os.name == 'posix', 'process groups require POSIX')
class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.base, self.candidate = self.root / 'baseline', self.root / 'candidate'
        for build in (self.base, self.candidate):
            (build / '_sdk').mkdir(parents=True)
            (build / 'tests').mkdir()
            (build / 'scuba-hostd').write_text('synthetic host: ' + build.name)
            self.write_sdk(build, COMPAT)
        self.set_binary('''import sys\nprint('kernel start failed')\nsys.exit(1)\n''')
        self.count = 0

    def write_sdk(self, build, metadata):
        with zipfile.ZipFile(build / '_sdk/sco-sdk-host-source-sc-2.4.0.zip', 'w') as z:
            z.writestr('sco-sdk-host-source-sc-2.4.0/COMPATIBILITY.json', json.dumps(metadata))

    def set_binary(self, body):
        for build in (self.base, self.candidate):
            binary = build / 'tests/test_net_sim'
            binary.write_text('#!/usr/bin/env python3\n' + body)
            binary.chmod(0o755)

    def run_harness(self, *extra):
        self.count += 1
        out = self.root / f'out{self.count}'
        result = subprocess.run([sys.executable, str(SCRIPT), '--baseline-build', str(self.base),
                                 '--candidate-build', str(self.candidate), '--runs', '1',
                                 '--timeout', '0.2', '--out', str(out), *extra],
                                capture_output=True, text=True, timeout=10)
        return result, json.loads((out / 'receipt.json').read_text())

    def test_startup_and_crash_are_inconclusive(self):
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(receipt['runs'][0]['classification'], 'inconclusive')
        self.set_binary('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(receipt['runs'][0]['classification'], 'inconclusive')

    def test_binary_mismatch_prevents_execution_and_explicit_binary_unifies(self):
        (self.candidate / 'tests/test_net_sim').write_text('different')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertIn('different SHA-256', receipt['reason'])
        self.assertEqual(receipt['runs'], [])
        binary = self.base / 'tests/test_net_sim'
        result, receipt = self.run_harness('--test-binary', str(binary))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(len(receipt['runs']), 2)
        self.assertEqual(receipt['inputs']['candidate']['test_binary']['path'], str(binary))
        self.assertEqual(receipt['inputs']['baseline']['test_binary']['sha256'],
                         receipt['inputs']['candidate']['test_binary']['sha256'])

    def test_incompatible_metadata_stops_before_execution(self):
        for change in ({'plugin_abi_version': '2'}, {'schema_version': 2},
                       {'source_commits': {'core': 'bad', 'game-sc': 'b' * 40}}):
            self.write_sdk(self.candidate, dict(COMPAT, **change))
            result, receipt = self.run_harness()
            self.assertEqual(result.returncode, 2)
            self.assertEqual(receipt['runs'], [])
            self.assertIn('input validation failed', receipt['reason'])

    def test_timeout_and_malformed_json(self):
        self.set_binary('import time\ntime.sleep(1)\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertTrue(receipt['runs'][0]['timed_out'])
        self.set_binary("import sys\nprint('peer condition timeout: {broken')\nsys.exit(1)\n")
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(receipt['runs'][0]['classification'], 'inconclusive')

    def test_noop_candidate_and_synthetic_control(self):
        self.set_binary('import sys\nif "baseline" in sys.argv[1]:\n print(' + repr(LOSS) + '); sys.exit(1)\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertEqual([r['classification'] for r in receipt['runs']], ['loss', 'inconclusive'])
        self.set_binary('import sys\nif "baseline" in sys.argv[1]:\n print(' + repr(LOSS) + '); sys.exit(1)\nprint(' + repr(SLOTS) + ')\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 0)
        self.assertEqual([r['classification'] for r in receipt['runs']], ['loss', 'pass'])

    def test_nonzero_candidate_fails(self):
        self.set_binary('import sys\nif "baseline" in sys.argv[1]:\n print(' + repr(LOSS) + '); sys.exit(1)\nprint("assertion failed"); sys.exit(1)\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(receipt['result'], 'failure')

    def test_identical_runtime_rejects_success_shaped_comparison_before_execution(self):
        (self.candidate / 'scuba-hostd').write_bytes((self.base / 'scuba-hostd').read_bytes())
        self.set_binary('import sys\nif "baseline" in sys.argv[1]:\n print(' + repr(LOSS) + '); sys.exit(1)\nprint(' + repr(SLOTS) + ')\n')
        result, receipt = self.run_harness()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(receipt['result'], 'inconclusive')
        self.assertIn('hostd binaries have identical SHA-256', receipt['reason'])
        self.assertEqual(receipt['runs'], [])


if __name__ == '__main__':
    unittest.main()
