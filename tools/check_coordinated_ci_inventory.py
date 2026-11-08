#!/usr/bin/env python3
"""Require the critical candidate CTests before treating a CI run as meaningful."""
import json
from pathlib import Path
import sys

REQUIRED = {
    'kernel_lifecycle_clean', 'kernel_lifecycle_refusal',
    'kernel_lifecycle_startup-retained', 'kernel_lifecycle_stop-retained',
    'hostd_peer_shutdown_order', 'sco_host_void_stop_fail_stops',
    'netsim_unclean_shutdown_is_terminal', 'netsim_two_peer_delivery',
    'product_profile_lifecycle', 'selected_profile_lifecycle',
    'designer_generated_plugins', 'designer_codegen_unit', 'studio_mcp_unit',
    'engine_integration',
}
WINDOWS_REQUIRED = {'engine_offscreen_smoke'}


def check(path, platform):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    tests = data.get('tests')
    if not isinstance(tests, list):
        raise SystemExit('CTest inventory has no test list')
    names = [test.get('name') for test in tests if isinstance(test, dict)]
    if any(not isinstance(name, str) or not name for name in names) or len(names) != len(tests):
        raise SystemExit('CTest inventory contains an invalid test')
    if len(set(names)) != len(names):
        raise SystemExit('CTest inventory contains duplicate names')
    required = REQUIRED | (WINDOWS_REQUIRED if platform == 'windows' else set())
    missing = sorted(required - set(names))
    if missing:
        raise SystemExit('Missing required CTests: ' + ', '.join(missing))
    for test in tests:
        if test['name'] in required:
            for prop in test.get('properties', []):
                if prop.get('name') == 'DISABLED' and str(prop.get('value')).upper() in {'TRUE', 'ON', '1', 'YES'}:
                    raise SystemExit('Required CTest is disabled: ' + test['name'])
    print(f'Verified {len(names)} registered CTests, including all {len(required)} required {platform} tests')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[2] not in ('linux', 'windows'):
        raise SystemExit('usage: check_coordinated_ci_inventory.py INVENTORY_JSON linux|windows')
    check(sys.argv[1], sys.argv[2])
