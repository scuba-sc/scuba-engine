#!/usr/bin/env python3
"""Fail closed if a coordinated CI configure selected the wrong profile/toolchain."""
import os
from pathlib import Path
import re
import sys


def check(build, platform):
    build = Path(build)
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        match = re.match(r'^([^#/:=]+):[^=]+=(.*)$', line)
        if match:
            cache[match.group(1)] = match.group(2)
    expected = {
        'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_GENERATOR': 'Ninja',
        'SCUBA_BUILD_ENGINE': 'ON', 'SCUBA_BUILD_TESTS': 'ON', 'SCUBA_WERROR': 'ON',
        'SCUBA_SDK_VERSION': '2.4.1-dev.1', 'SCUBA_SDK_RELEASE_TAG': '',
        'SCUBA_SDK_HOST_SOURCE_SHA256': os.environ['HOST_SHA256'],
        'SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256': os.environ['HOST_SHA256'],
        'SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT': os.environ['CORE_COMMIT'],
        'SCUBA_CORE_LIFECYCLE_CANDIDATE': 'ON', 'SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL': 'OFF',
        'SCUBA_SDK_CHECKED_LIFECYCLE': '1',
    }
    for key, value in expected.items():
        if cache.get(key) != value:
            raise SystemExit(f'{key}: expected {value!r}, got {cache.get(key)!r}')
    if Path(cache.get('SCUBA_SDK_ARCHIVE_DIR', '')).resolve() != Path(os.environ['SCUBA_DEV_SDK_ARCHIVE_DIR']).resolve():
        raise SystemExit('configured archive directory differs from verified artifact directory')
    compiler_files = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    if len(compiler_files) != 1:
        raise SystemExit('expected exactly one CMake C++ compiler identity file')
    compiler = compiler_files[0].read_text(encoding='utf-8')
    identity = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)', compiler)
    width = re.search(r'set\(CMAKE_CXX_SIZEOF_DATA_PTR "([^"]+)"\)', compiler)
    accepted = {'windows': {'MSVC'}, 'linux': {'GNU', 'Clang'}}[platform]
    if not identity or identity.group(1) not in accepted or not width or width.group(1) != '8':
        raise SystemExit(f'unexpected {platform} compiler identity or pointer size')
    print(f'Verified coordinated profile and {platform} compiler {identity.group(1)}, 64-bit')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[2] not in ('linux', 'windows'):
        raise SystemExit('usage: check_coordinated_ci_cache.py BUILD_DIR linux|windows')
    check(sys.argv[1], sys.argv[2])
