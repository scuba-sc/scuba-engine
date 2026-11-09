#!/usr/bin/env python3
"""Isolated, unpublished public archive consumer proof (mock checker only)."""
import argparse
import hashlib
import json
import posixpath
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from verify_dev_plugin_sdk import PREFIX, VERSION, verify


def run(args, cwd=None):
    result = subprocess.run([str(a) for a in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    result.check_returncode()
    return result.stdout


def normalized(value):
    """Compare both Windows and POSIX spellings conservatively on either host."""
    return str(value).replace('\\', '/').casefold()


def resolved_evidence_path(value, directory):
    """Resolve Ninja paths lexically, including foreign Windows drive/UNC paths."""
    path = normalized(value)
    if re.match(r'^[a-z]:[^/]', path) or re.fullmatch(r'[a-z]:', path):
        raise ValueError('ambiguous drive-relative dependency path')
    if not (path.startswith('/') or re.match(r'^[a-z]:/', path)):
        path = normalized(directory).rstrip('/') + '/' + path
    return posixpath.normpath(path)


def validate_evidence(commands_text, dependencies, source, sdk, build):
    commands = json.loads(commands_text)
    if not isinstance(commands, list) or not commands:
        raise ValueError('missing compile commands')
    source_path = posixpath.normpath(normalized(source)).rstrip('/')

    def reject_source(value):
        if source_path in normalized(value):
            raise ValueError('consumer evidence references original source checkout')

    expected_outputs = set()
    for entry in commands:
        if not isinstance(entry, dict) or not all(
                isinstance(entry.get(key), str) and entry[key]
                for key in ('directory', 'file', 'output')):
            raise ValueError('invalid compile command entry')
        invocation = entry.get('arguments', entry.get('command'))
        if isinstance(invocation, list):
            if not invocation or not all(isinstance(arg, str) for arg in invocation):
                raise ValueError('invalid compile arguments')
            fields = invocation
        elif isinstance(invocation, str) and invocation:
            fields = [invocation]
        else:
            raise ValueError('missing compile invocation')
        for field in [entry['directory'], entry['file'], entry['output'], *fields]:
            reject_source(field)
        directory = resolved_evidence_path(entry['directory'], build)
        for field in ('file', 'output'):
            reject_source(resolved_evidence_path(entry[field], directory))
        expected_outputs.add(resolved_evidence_path(entry['output'], directory))

    records = {}
    current = None
    for line in dependencies.splitlines():
        if not line.strip():
            continue
        reject_source(line)
        match = re.fullmatch(r'(.+): #deps ([0-9]+), deps mtime [0-9]+ \(VALID\)', line)
        if match:
            output = resolved_evidence_path(match[1], build)
            reject_source(output)
            if output in records:
                raise ValueError('duplicate dependency record')
            current = (int(match[2]), [])
            records[output] = current
        elif line.startswith('    ') and current is not None:
            path = resolved_evidence_path(line.strip(), build)
            reject_source(path)
            current[1].append(path)
        else:
            raise ValueError('invalid or stale dependency evidence')
    if not records or not expected_outputs.issubset(records):
        raise ValueError('missing compiler dependency records')
    for count, paths in records.values():
        if not count or count != len(paths):
            raise ValueError('missing actual compiler dependencies')
    public_header = resolved_evidence_path('include/sco_api.h', sdk)
    if not any(public_header in paths for _, paths in records.values()):
        raise ValueError('dependency evidence omits public SDK header')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('sha256')
    parser.add_argument('core_commit')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    count = verify(args.archive, args.sha256, args.core_commit)
    source = Path(__file__).resolve().parents[1]
    # No source-tree build, cache reuse, host-source extraction, or deletion of inputs.
    with tempfile.TemporaryDirectory(prefix='development-plugin-consumer-') as temporary:
        # Windows TEMP may use an 8.3 alias while CMake records canonical paths.
        root = Path(temporary).resolve()
        consumer = root / 'consumer'
        shutil.copytree(source / 'plugins', consumer / 'plugins')
        (consumer / 'cmake').mkdir()
        for name in ('ScubaPluginSdk.cmake', 'ScubaWarnings.cmake'):
            shutil.copy2(source / 'cmake' / name, consumer / 'cmake' / name)
        with zipfile.ZipFile(args.archive) as kit:
            kit.extractall(root / 'sdk')
        sdk = root / 'sdk' / PREFIX[:-1]
        build = root / 'build'
        configure = ['cmake', '-S', consumer / 'plugins', '-B', build, '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', '-DSCUBA_BUILD_TESTS=OFF',
                     '-DSCUBA_DEVELOPMENT_PLUGIN_SDK=ON', f'-DSCUBA_PLUGIN_SDK_ROOT={sdk}']
        # A development archive must not silently enter the published stable path.
        rejected = subprocess.run([str(a) for a in configure if a != '-DSCUBA_DEVELOPMENT_PLUGIN_SDK=ON'],
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if rejected.returncode == 0 or 'must be sco-sdk-2.4.0' not in rejected.stdout:
            raise SystemExit('stable consumer did not reject development SDK for the expected reason')
        shutil.rmtree(build)
        log = run(configure)
        log += run(['cmake', '--build', build, '--target', 'scuba_plugins', 'sco-plugin-check', '--parallel', '4'])
        commands = (build / 'compile_commands.json').read_text()
        dependencies = run(['ninja', '-C', build, '-t', 'deps'])
        validate_evidence(commands, dependencies, source, sdk, build)
        checker = build / ('sco-plugin-check.exe' if __import__('os').name == 'nt' else 'sco-plugin-check')
        checks = []
        for plugin in ('mp_chat', 'org_social', 'diegetic_asop', 'diegetic_shop_inv'):
            for variant in ('full', 'short-tables'):
                command = [checker, build / 'runtime' / plugin]
                if variant == 'short-tables':
                    command.append('--short-tables')
                output = run(command)
                if 'OK: 0 failure(s)' not in output:
                    raise SystemExit(f'{plugin} {variant}: missing checker success')
                log += output
                checks.append(f'{plugin}/{variant}')
        args.evidence.mkdir(parents=True, exist_ok=True)
        (args.evidence / 'compile_commands.json').write_text(commands)
        (args.evidence / 'dependencies.txt').write_text(dependencies)
        (args.evidence / 'consumer.log').write_text(log)
        source_inventory = {p.relative_to(consumer).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sorted(consumer.rglob('*')) if p.is_file()}
        (args.evidence / 'receipt.json').write_text(json.dumps({
            'profile': 'legacy', 'sdk_version': VERSION, 'published': False,
            'core_commit': args.core_commit, 'archive_sha256': args.sha256,
            'verified_entries': count, 'checks': checks,
            'consumer_source_sha256': source_inventory,
            'evidence_scope': 'public archive build and mock-host plugin execution; no game or native-backend acceptance',
        }, indent=2) + '\n')


if __name__ == '__main__':
    main()
