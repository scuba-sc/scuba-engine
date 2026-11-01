#!/usr/bin/env python3
"""Compare eight-peer regressions; retain failures and never retry to green.

Linux /proc/net/snmp counts are aggregate counters in the current network
namespace, not per-process attribution. Run without unrelated UDP workloads.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import time
import zipfile


class StopRequested(Exception):
    def __init__(self, signum):
        self.signum = signum


def stop_requested(signum, _frame):
    raise StopRequested(signum)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def udp_counters():
    try:
        lines = Path('/proc/net/snmp').read_text().splitlines()
        for header, values in zip(lines, lines[1:]):
            if header.startswith('Udp:') and values.startswith('Udp:'):
                return dict(zip(header.split()[1:], map(int, values.split()[1:]), strict=True))
    except (OSError, ValueError):
        # These optional aggregate counters are unavailable on some systems.
        pass
    return None


def identify(build, test_binary):
    archive = build / '_sdk/sco-sdk-host-source-sc-2.4.0.zip'
    with zipfile.ZipFile(archive) as kit:
        compatibility = json.loads(kit.read('sco-sdk-host-source-sc-2.4.0/COMPATIBILITY.json'))
    return {'build': str(build), 'sdk_archive_sha256': digest(archive),
            'sdk_compatibility': compatibility,
            'test_binary': {'path': str(test_binary), 'sha256': digest(test_binary)},
            'hostd': {'path': str(build / 'scuba-hostd'), 'sha256': digest(build / 'scuba-hostd')}}


def validate_inputs(inputs):
    expected = ('core_sdk_version', 'game_overlay_version', 'plugin_abi_version')
    for label, item in inputs.items():
        c = item['sdk_compatibility']
        if not isinstance(c, dict) or type(c.get('schema_version')) is not int or c['schema_version'] != 1 or c.get('profile') != 'host-source-sc':
            raise ValueError(f'{label}: unsupported SDK schema or profile')
        for key in expected:
            pattern = r'[0-9]+\.[0-9]+' if key == 'plugin_abi_version' else r'[0-9]+\.[0-9]+\.[0-9]+'
            if not isinstance(c.get(key), str) or not re.fullmatch(pattern, c[key]):
                raise ValueError(f'{label}: invalid SDK {key}')
        if 'base_core_sha256' not in c or c['base_core_sha256'] is not None:
            raise ValueError(f'{label}: invalid SDK base_core_sha256')
        commits = c.get('source_commits')
        if not isinstance(commits, dict) or set(commits) != {'core', 'game-sc'} or any(
                not isinstance(commits.get(key), str) or not re.fullmatch(r'[0-9a-f]{40}', commits[key])
                for key in ('core', 'game-sc')):
            raise ValueError(f'{label}: invalid SDK source coordinates')
    baseline, candidate = inputs['baseline'], inputs['candidate']
    for key in expected:
        if baseline['sdk_compatibility'][key] != candidate['sdk_compatibility'][key]:
            raise ValueError(f'incompatible SDK {key}')
    if baseline['test_binary']['sha256'] != candidate['test_binary']['sha256']:
        raise ValueError('selected test binaries have different SHA-256 values')


SLOT = re.compile(r'^slot=([1-8]) reliable_received=224 unreliable_received=224 unreliable_loss=0$')
INFRA_ERROR = re.compile(r'(?:kernel start failed|session (?:start|event subscription) failed|'
                         r'peer (?:launch failed|input pipe failed|output pipe failed|disconnected|exited|'
                         r'control stream is closed|response timeout)|probe send refused|'
                         r'synthetic probe is not loaded|sandbox data root already exists|'
                         r'peer slot is not running|AddressSanitizer|Segmentation fault)', re.I)
CHECK_FAILURE = re.compile(r'(?:CHECK failed:|assertion(?:\s+|[^\n]*?)failed)', re.I)


def classify(label, code, timed_out, log):
    """Classify captured output; synthetic examples are not runtime evidence."""
    if timed_out:
        return 'inconclusive', 'process timed out'
    if code is None or code < 0 or code >= 128 or INFRA_ERROR.search(log):
        return 'inconclusive', 'process crash, startup, or API failure'
    if CHECK_FAILURE.search(log):
        return (('failure', 'candidate reported a failed check') if label == 'candidate'
                else ('inconclusive', 'baseline reported a failed check'))
    diagnostics = [line.split('peer condition timeout: ', 1)[1]
                   for line in log.splitlines() if 'peer condition timeout: ' in line]
    if diagnostics:
        try:
            probes = [json.loads(line) for line in diagnostics]
            for entry in probes:
                probe = entry['probe']
                counts = [probe[k] for k in ('reliableSent', 'unreliableSent',
                                             'reliableReceived', 'unreliableReceived')]
                if (entry['active'] is not True or entry['members'] != 8 or
                        any(type(n) is not int for n in counts) or
                        counts[:3] != [32, 32, 224] or not 0 <= counts[3] < 224):
                    raise ValueError('unexpected probe counts')
        except (ValueError, KeyError, TypeError):
            return 'inconclusive', 'malformed or unexpected peer condition timeout diagnostic'
        if code == 0:
            return 'inconclusive', 'successful exit accompanied by timeout diagnostic'
        if label == 'baseline' and code != 1:
            return 'inconclusive', 'unexpected baseline exit code for loss diagnostic'
        return ('loss' if label == 'baseline' else 'failure'), 'peer condition timeout with unreliable loss'
    if code != 0:
        return (('failure', 'candidate exited nonzero') if label == 'candidate'
                else ('inconclusive', 'baseline exited without a valid loss diagnostic'))
    slots = [SLOT.fullmatch(line) for line in log.splitlines() if line.startswith('slot=')]
    if len(slots) != 8 or any(s is None for s in slots) or {int(s.group(1)) for s in slots} != set(range(1, 9)):
        return 'inconclusive', 'missing, malformed, or duplicate slot results'
    return 'pass', 'all eight slots received 224 reliable and unreliable messages'


def kill_group(process):
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        # The process group can exit between wait timing out and killpg.
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--candidate-build', type=Path, required=True)
    parser.add_argument('--test-binary', type=Path, help='Use this same regression binary for both builds')
    parser.add_argument('--runs', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120,
                        help='Per-run timeout, up to the existing 120-second CTest limit')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'posix' or args.runs < 1 or not 0 < args.timeout <= 120:
        parser.error('requires POSIX process groups, timeout in (0, 120], and at least one run')
    signal.signal(signal.SIGTERM, stop_requested)
    builds = {'baseline': args.baseline_build.resolve(), 'candidate': args.candidate_build.resolve()}
    args.out.mkdir(parents=True, exist_ok=False)
    receipt = args.out / 'receipt.json'
    manifest = {'platform': platform.platform(), 'inputs': {},
                'counter_scope': 'aggregate current network namespace; unrelated UDP may contribute',
                'runs': [], 'result': 'inconclusive', 'reason': 'comparison not completed'}

    def save():
        receipt.write_text(json.dumps(manifest, indent=2) + '\n')

    save()
    try:
        for label, build in builds.items():
            binary = args.test_binary.resolve() if args.test_binary else build / 'tests/test_net_sim'
            manifest['inputs'][label] = identify(build, binary)
        validate_inputs(manifest['inputs'])
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        manifest['reason'] = f'input validation failed: {error}'
        save()
        print(manifest['reason'], flush=True)
        return 2
    save()
    for run in range(1, args.runs + 1):
        for label, build in builds.items():
            command = [manifest['inputs'][label]['test_binary']['path'], str(build / 'scuba-hostd'), 'bounds']
            before = udp_counters()
            started_utc = datetime.now(timezone.utc).isoformat()
            start = time.monotonic()
            log = args.out / f'{label}-{run}.log'
            timed_out = False
            interruption = None
            interrupt_exit = 1
            code = None
            with log.open('wb') as stream:
                try:
                    process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                except OSError as error:
                    stream.write(f'process launch failed: {error}\n'.encode())
                else:
                    try:
                        code = process.wait(timeout=args.timeout)
                    except subprocess.TimeoutExpired:
                        timed_out = True
                        kill_group(process)
                        code = process.wait()
                    except (Exception, KeyboardInterrupt) as error:
                        interruption = (signal.Signals(error.signum).name if isinstance(error, StopRequested)
                                        else type(error).__name__)
                        interrupt_exit = (128 + error.signum if isinstance(error, StopRequested)
                                          else 130 if isinstance(error, KeyboardInterrupt) else 1)
                        kill_group(process)
                        code = process.wait()
            after = udp_counters()
            delta = ({k: after[k] - before[k] for k in before if k in after}
                     if before is not None and after is not None else None)
            evidence, reason = classify(label, code, timed_out, log.read_text(errors='replace'))
            row = {'profile': label, 'run': run, 'command': command, 'exit_code': code,
                   'started_utc': started_utc, 'ended_utc': datetime.now(timezone.utc).isoformat(),
                   'interruption': interruption, 'timeout_seconds': args.timeout,
                   'timed_out': timed_out, 'seconds': time.monotonic() - start,
                   'udp_before': before, 'udp_after': after, 'udp_delta': delta,
                   'log': str(log), 'log_sha256': digest(log), 'classification': evidence, 'reason': reason}
            manifest['runs'].append(row)
            if interruption:
                manifest['reason'] = f'interrupted: {interruption}'
            save()
            print(json.dumps({'profile': label, 'run': run, 'exit_code': code,
                              'classification': evidence,
                              'RcvbufErrors': delta.get('RcvbufErrors') if delta else None}), flush=True)
            if interruption:
                return interrupt_exit
    results = [r['classification'] for r in manifest['runs']]
    if 'failure' in results:
        manifest['result'], manifest['reason'], exit_code = 'failure', 'candidate failed', 1
    elif 'inconclusive' in results:
        manifest['result'], manifest['reason'], exit_code = 'inconclusive', 'one or more runs lacked valid evidence', 2
    elif 'loss' not in results:
        manifest['result'], manifest['reason'], exit_code = 'inconclusive', 'no baseline unreliable loss observed', 2
    else:
        manifest['result'], manifest['reason'], exit_code = 'pass', 'baseline loss reproduced and all candidate runs passed', 0
    save()
    print(manifest['reason'], flush=True)
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
