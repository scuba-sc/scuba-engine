#!/usr/bin/env python3
"""Positive and negative controls for the CI profile/toolchain guard."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/check_coordinated_ci_cache.py'


class CoordinatedCacheGuard(unittest.TestCase):
    def test_profile_and_compiler_controls(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build, sdk = root / 'build', root / 'sdk'
            build.mkdir()
            sdk.mkdir()
            compiler_file = build / 'CMakeFiles/3.30/CMakeCXXCompiler.cmake'
            compiler_file.parent.mkdir(parents=True)
            sha, commit = 'a' * 64, 'b' * 40
            values = {
                'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_GENERATOR': 'Ninja',
                'SCUBA_BUILD_ENGINE': 'ON', 'SCUBA_BUILD_TESTS': 'ON', 'SCUBA_WERROR': 'ON',
                'SCUBA_SDK_VERSION': '2.4.1-dev.1', 'SCUBA_SDK_RELEASE_TAG': '',
                'SCUBA_SDK_HOST_SOURCE_SHA256': sha, 'SCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256': sha,
                'SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT': commit,
                'SCUBA_CORE_LIFECYCLE_CANDIDATE': 'ON', 'SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL': 'OFF',
                'SCUBA_SDK_CHECKED_LIFECYCLE': '1', 'SCUBA_SDK_ARCHIVE_DIR': str(sdk),
                'SCUBA_REQUIRE_GENERATED_LUA': 'ON', 'SCUBA_LUA_EXECUTABLE': sys.executable,
            }
            env = os.environ | {'HOST_SHA256': sha, 'CORE_COMMIT': commit,
                                'SCUBA_DEV_SDK_ARCHIVE_DIR': str(sdk)}
            def run(changes=None, compiler='MSVC', width='8', platform='windows'):
                current = values | (changes or {})
                (build / 'CMakeCache.txt').write_text(''.join(f'{k}:STRING={v}\n' for k, v in current.items()))
                compiler_file.write_text(f'set(CMAKE_CXX_COMPILER_ID "{compiler}")\nset(CMAKE_CXX_SIZEOF_DATA_PTR "{width}")\n')
                return subprocess.run([sys.executable, str(SCRIPT), str(build), platform],
                                      env=env, capture_output=True, text=True).returncode
            self.assertEqual(run(), 0)
            self.assertEqual(run(compiler='GNU', platform='linux'), 0)
            for changes in ({'SCUBA_SDK_VERSION': '2.4.0'}, {'SCUBA_SDK_RELEASE_TAG': 'sdk-v2.4.0'},
                            {'SCUBA_CORE_LIFECYCLE_CANDIDATE': 'OFF'},
                            {'SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL': 'ON'},
                            {'SCUBA_SDK_HOST_SOURCE_SHA256': '0' * 64},
                            {'SCUBA_CORE_LIFECYCLE_SOURCE_COMMIT': '0' * 40},
                            {'SCUBA_SDK_ARCHIVE_DIR': str(root)},
                            {'SCUBA_SDK_CHECKED_LIFECYCLE': ''},
                            {'SCUBA_REQUIRE_GENERATED_LUA': 'OFF'},
                            {'SCUBA_LUA_EXECUTABLE': ''},
                            {'SCUBA_LUA_EXECUTABLE': 'SCUBA_LUA_EXECUTABLE-NOTFOUND'},
                            {'SCUBA_LUA_EXECUTABLE': str(root / 'missing.exe')}):
                with self.subTest(changes=changes):
                    self.assertNotEqual(run(changes), 0)
            self.assertNotEqual(run(compiler='Clang'), 0)
            self.assertNotEqual(run(width='4'), 0)


if __name__ == '__main__':
    unittest.main()
