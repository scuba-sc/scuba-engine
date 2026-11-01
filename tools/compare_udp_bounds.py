#!/usr/bin/env python3
"""Compare unchanged eight-peer regression binaries; retain failures, never retry to green.

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
        pass
    return None


def identify(build):
    archive = build / '_sdk/sco-sdk-host-source-sc-2.4.0.zip'
    with zipfile.ZipFile(archive) as kit:
        compatibility = json.loads(kit.read('sco-sdk-host-source-sc-2.4.0/COMPATIBILITY.json'))
    return {'build': str(build), 'sdk_archive_sha256': digest(archive),
            'sdk_compatibility': compatibility,
            'binaries': {str(p.relative_to(build)): digest(p) for p in
                         [build / 'tests/test_net_sim', build / 'scuba-hostd']}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--candidate-build', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120,
                        help='Per-run timeout, matching the existing CTest limit by default')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'posix' or args.runs < 1 or args.timeout <= 0:
        parser.error('requires POSIX process groups, positive timeout and at least one run')
    signal.signal(signal.SIGTERM, stop_requested)
    builds = {'baseline': args.baseline_build.resolve(), 'candidate': args.candidate_build.resolve()}
    manifest = {'platform': platform.platform(), 'inputs': {k: identify(v) for k, v in builds.items()},
                'counter_scope': 'aggregate current network namespace; unrelated UDP may contribute',
                'runs': []}
    args.out.mkdir(parents=True, exist_ok=False)
    for run in range(1, args.runs + 1):
        for label, build in builds.items():
            command = [str(build / 'tests/test_net_sim'), str(build / 'scuba-hostd'), 'bounds']
            before = udp_counters()
            started_utc = datetime.now(timezone.utc).isoformat()
            start = time.monotonic()
            log = args.out / f'{label}-{run}.log'
            timed_out = False
            interruption = None
            interrupt_exit = 1
            with log.open('wb') as stream:
                process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    code = process.wait()
                except BaseException as error:
                    interruption = (signal.Signals(error.signum).name if isinstance(error, StopRequested)
                                    else type(error).__name__)
                    interrupt_exit = (128 + error.signum if isinstance(error, StopRequested)
                                      else 130 if isinstance(error, KeyboardInterrupt) else 1)
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    code = process.wait()
            after = udp_counters()
            delta = ({k: after[k] - before[k] for k in before if k in after}
                     if before is not None and after is not None else None)
            row = {'profile': label, 'run': run, 'command': command, 'exit_code': code,
                   'started_utc': started_utc, 'ended_utc': datetime.now(timezone.utc).isoformat(),
                   'interruption': interruption, 'timeout_seconds': args.timeout,
                   'timed_out': timed_out, 'seconds': time.monotonic() - start,
                   'udp_before': before, 'udp_after': after, 'udp_delta': delta,
                   'log': str(log), 'log_sha256': digest(log)}
            manifest['runs'].append(row)
            (args.out / 'receipt.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(json.dumps({'profile': label, 'run': run, 'exit_code': code,
                              'RcvbufErrors': delta.get('RcvbufErrors') if delta else None}), flush=True)
            if interruption:
                raise SystemExit(interrupt_exit)
    candidate_ok = all(r['exit_code'] == 0 for r in manifest['runs'] if r['profile'] == 'candidate')
    reproduced = any(r['exit_code'] != 0 and not r['timed_out']
                     for r in manifest['runs'] if r['profile'] == 'baseline')
    if not candidate_ok:
        raise SystemExit(1)
    if not reproduced:
        print('No baseline failure observed: delivery-fix comparison is inconclusive.')
        raise SystemExit(2)


if __name__ == '__main__':
    main()
