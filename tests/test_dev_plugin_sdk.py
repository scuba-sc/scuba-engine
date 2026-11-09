"""Negative controls use synthetic ZIPs, never evidence of a real SDK build."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_dev_plugin_sdk import PREFIX, VERSION, verify

COMMIT = 'a' * 40


class DevelopmentPluginArchive(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / (PREFIX[:-1] + '.zip')
        self.entries = {
            'VERSION': (VERSION + '\n').encode(),
            'COMPATIBILITY.json': json.dumps({
                'profile': 'legacy', 'core_sdk_version': VERSION, 'plugin_abi_version': '1.1',
                'source_commits': {'core': COMMIT, 'game-sc': COMMIT}}).encode(),
            'include/sco_api.h': b'public fixture', 'cmake/sco-plugin.cmake': b'fixture',
            'tools/sco-plugin-check.c': b'fixture',
        }

    def write(self, corrupt=False, omit=False, extra=None):
        sums = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                       for name, data in self.entries.items())
        with zipfile.ZipFile(self.path, 'w') as kit:
            for name, data in self.entries.items():
                kit.writestr(PREFIX + name, data + (b'x' if corrupt else b''))
            kit.writestr(PREFIX + 'SHA256SUMS', sums.split('\n', 1)[1] if omit else sums)
            if extra:
                # Windows ZipInfo normalizes backslashes before writing. Create
                # equal-length safe bytes, then mutate both ZIP filename records.
                placeholder = extra.replace('\\', '~').replace('\0', '~')
                kit.writestr(placeholder, b'extra')
        if extra and any(char in extra for char in ('\\', '\0')):
            original = placeholder.encode('utf-8')
            intended = extra.encode('utf-8')
            data = self.path.read_bytes()
            self.assertEqual(len(original), len(intended))
            self.assertEqual(data.count(original), 2)  # local + central directory
            data = data.replace(original, intended)
            self.assertEqual(data.count(intended), 2)
            self.path.write_bytes(data)
            with zipfile.ZipFile(self.path) as kit:
                self.assertIn(extra, [info.orig_filename for info in kit.infolist()])
        return hashlib.sha256(self.path.read_bytes()).hexdigest()

    def test_valid(self):
        self.assertEqual(verify(self.path, self.write(), COMMIT), 5)

    def test_wrong_hash(self):
        self.write()
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            verify(self.path, '0' * 64, COMMIT)

    def test_wrong_source(self):
        with self.assertRaisesRegex(ValueError, 'source/version/profile'):
            verify(self.path, self.write(), 'b' * 40)

    def test_wrong_profile_and_version_and_abi(self):
        original = json.loads(self.entries['COMPATIBILITY.json'])
        for field, value in [('profile', 'host-source-sc'), ('core_sdk_version', '2.4.0'),
                             ('plugin_abi_version', '2.0')]:
            with self.subTest(field=field):
                self.entries['COMPATIBILITY.json'] = json.dumps(dict(original, **{field: value})).encode()
                with self.assertRaisesRegex(ValueError, 'source/version/profile'):
                    verify(self.path, self.write(), COMMIT)

    def test_wrong_version_file(self):
        self.entries['VERSION'] = b'2.4.0\n'
        with self.assertRaisesRegex(ValueError, 'source/version/profile'):
            verify(self.path, self.write(), COMMIT)

    def test_inventory_incomplete(self):
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            verify(self.path, self.write(omit=True), COMMIT)

    def test_corrupt_entry(self):
        self.entries['include/sco_api.h'] = b'original'
        digest = self.write()
        # Rewrite without fixing the entry's checksum, while re-binding outer hash.
        with zipfile.ZipFile(self.path) as kit:
            entries = {i.filename: kit.read(i) for i in kit.infolist()}
        entries[PREFIX + 'include/sco_api.h'] = b'changed'
        with zipfile.ZipFile(self.path, 'w') as kit:
            for name, data in entries.items():
                kit.writestr(name, data)
        with self.assertRaisesRegex(ValueError, 'entry checksum'):
            verify(self.path, hashlib.sha256(self.path.read_bytes()).hexdigest(), COMMIT)

    def test_unsafe_entries(self):
        for extra in ('../escape', PREFIX + '../escape', PREFIX + '/absolute', PREFIX + 'a\\b',
                      PREFIX + 'C:stream', PREFIX + './alias', PREFIX + 'nul\0suffix'):
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, 'unsafe'):
                verify(self.path, self.write(extra=extra), COMMIT)

    def test_windows_reader_normalization(self):
        from unittest.mock import patch
        original = zipfile.ZipInfo.__init__

        def windows_read(info, filename='NoName', date_time=(1980, 1, 1, 0, 0, 0)):
            original(info, filename, date_time)
            info.filename = info.filename.replace('\\', '/')

        extra = PREFIX + 'a\\b'
        digest = self.write(extra=extra)
        with patch.object(zipfile.ZipInfo, '__init__', windows_read):
            with zipfile.ZipFile(self.path) as kit:
                self.assertIn(extra, [info.orig_filename for info in kit.infolist()])
                self.assertNotIn(extra, kit.namelist())
            with self.assertRaisesRegex(ValueError, 'unsafe archive entry normalization'):
                verify(self.path, digest, COMMIT)

    def test_private_headers(self):
        self.entries['include/sco/internal/host.h'] = b'private'
        with self.assertRaisesRegex(ValueError, 'private host'):
            verify(self.path, self.write(), COMMIT)

    def test_missing_required(self):
        del self.entries['include/sco_api.h']
        with self.assertRaisesRegex(ValueError, 'missing public'):
            verify(self.path, self.write(), COMMIT)

    def test_extra_uninventoried(self):
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            verify(self.path, self.write(extra=PREFIX + 'extra'), COMMIT)

    def test_duplicate(self):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            digest = self.write(extra=PREFIX + 'VERSION')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            verify(self.path, digest, COMMIT)

    def test_symlink(self):
        self.write()
        with zipfile.ZipFile(self.path, 'a') as kit:
            info = zipfile.ZipInfo(PREFIX + 'link')
            info.external_attr = 0o120777 << 16
            kit.writestr(info, 'target')
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            verify(self.path, hashlib.sha256(self.path.read_bytes()).hexdigest(), COMMIT)



class ConsumerEvidence(unittest.TestCase):
    def setUp(self):
        from run_dev_plugin_consumer import validate_evidence
        self.validate = validate_evidence
        self.commands = [{'directory': 'C:/temp/build', 'file': 'C:/temp/consumer/plugin.c',
                          'output': 'C:/temp/build/plugin.obj',
                          'command': 'cl /IC:/temp/sdk/include /c C:/temp/consumer/plugin.c'}]
        self.deps = ('plugin.obj: #deps 1, deps mtime 123 (VALID)\n'
                     '    C:/temp/sdk/include/sco_api.h\n')

    def check(self, deps=None):
        self.validate(json.dumps(self.commands), self.deps if deps is None else deps,
                      r'C:\Original\Engine', 'C:/temp/sdk', 'C:/temp/build')

    def test_valid_windows_spellings(self):
        self.check(self.deps.replace('C:/temp/sdk', r'c:\TEMP\SDK'))

    def test_relative_public_header(self):
        for path in ('../sdk/include/sco_api.h', r'..\SDK\include\sco_api.h',
                     '../sdk/other/../include/sco_api.h'):
            with self.subTest(path=path):
                self.check(self.deps.replace('C:/temp/sdk/include/sco_api.h', path))

    def test_relative_original_source_rejected(self):
        for path in ('../../Original/Engine/private.h', r'..\..\ORIGINAL\engine\private.h'):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'original source'):
                self.check(self.deps.replace('C:/temp/sdk/include/sco_api.h', path))

    def test_absolute_output_paths(self):
        for path in ('C:/temp/build/plugin.obj', r'c:\TEMP\build\plugin.obj'):
            with self.subTest(path=path):
                self.check(self.deps.replace('plugin.obj:', path + ':'))

    def test_relative_original_output_rejected(self):
        with self.assertRaisesRegex(ValueError, 'original source'):
            self.check(self.deps.replace('plugin.obj:', '../../Original/Engine/plugin.obj:'))

    def test_unc_paths(self):
        from run_dev_plugin_consumer import resolved_evidence_path
        self.assertEqual(resolved_evidence_path(r'\\Server\Share\dir\..\file.h', 'C:/temp/build'),
                         '//server/share/file.h')
        commands = json.dumps([{
            'directory': '//server/share/build', 'file': '../consumer/plugin.c',
            'output': '//server/share/build/plugin.obj', 'arguments': ['cl', '../consumer/plugin.c']}])
        deps = ('//server/share/build/plugin.obj: #deps 1, deps mtime 123 (VALID)\n'
                '    //server/share/sdk/include/sco_api.h\n')
        self.validate(commands, deps, '//server/original/engine',
                      '//server/share/sdk', '//server/share/build')

    def test_missing_dependencies(self):
        for deps in ('', 'ninja: no work to do.\n',
                     'plugin.obj: #deps 0, deps mtime 123 (VALID)\n',
                     'plugin.obj: #deps 1, deps mtime 123 (VALID)\n',
                     self.deps.replace('VALID', 'STALE'),
                     self.deps.replace('plugin.obj', 'other.obj'),
                     self.deps.replace('sco_api.h', 'unrelated.h')):
            with self.subTest(deps=deps), self.assertRaises(ValueError):
                self.check(deps)

    def test_source_dependencies_slash_case(self):
        for path in ('c:/ORIGINAL/engine/private.h', r'C:\original\ENGINE\private.h'):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'original source'):
                self.check(self.deps.replace('C:/temp/sdk/include/sco_api.h', path))

    def test_source_compile_fields_slash_case(self):
        for field in ('command', 'directory', 'file', 'output', 'arguments'):
            for path in ('c:/ORIGINAL/engine/private.h', r'C:\original\ENGINE\private.h'):
                with self.subTest(field=field, path=path):
                    old = self.commands[0].copy()
                    self.commands[0][field] = [path] if field == 'arguments' else path
                    with self.assertRaisesRegex(ValueError, 'original source'):
                        self.check()
                    self.commands[0] = old

    def test_relative_compiler_path_options(self):
        options = ('-I', '-isystem', '-iquote', '-include', '-imacros', '-idirafter',
                   '-isysroot', '--sysroot', '/I', '/FI', '/external:I')
        for option in options:
            for joined in (True, False):
                for windows in (True, False):
                    path = r'..\..\Original\Engine\private space' if windows else '../../Original/Engine/private space'
                    compiler = 'cl' if windows else 'cc'
                    arguments = [compiler, option + path] if joined else [compiler, option, path]
                    for invocation in (arguments, ' '.join('"' + arg + '"' for arg in arguments)):
                        with self.subTest(option=option, joined=joined, invocation=invocation):
                            self.commands[0].pop('arguments', None)
                            key = 'arguments' if isinstance(invocation, list) else 'command'
                            self.commands[0][key] = invocation
                            with self.assertRaisesRegex(ValueError, 'original source'):
                                self.check()

    def test_safe_relative_compiler_paths(self):
        for invocation in (['cc', '-I../sdk/include', '-include', '../consumer/local.h'],
                           r'cl /I"..\sdk\include space" /FI "..\consumer\local.h"',
                           'cc -isystem "../sdk/include space" -iquote../consumer',
                           ['cl', '/external:I../sdk/include', '/FI../consumer/local.h']):
            with self.subTest(invocation=invocation):
                self.commands[0].pop('arguments', None)
                key = 'arguments' if isinstance(invocation, list) else 'command'
                self.commands[0][key] = invocation
                self.check()

    def test_composed_prefix_options_rejected(self):
        for invocation in (['cc', '-iprefix', '../../Original/', '-iwithprefix', 'Engine/private'],
                           'cc -iprefix../../Original/ -iwithprefixbefore Engine/private',
                           ['cc', '-iwithprefixbefore', '../sdk']):
            with self.subTest(invocation=invocation):
                self.commands[0].pop('arguments', None)
                key = 'arguments' if isinstance(invocation, list) else 'command'
                self.commands[0][key] = invocation
                with self.assertRaisesRegex(ValueError, 'unsupported composed'):
                    self.check()

    def test_posix_command_escaping_rejected(self):
        self.commands[0]['command'] = r'cc -I../../Origina\l/Engine/private'
        with self.assertRaisesRegex(ValueError, 'unsupported POSIX command escaping'):
            self.check()

    def test_recorded_msvc_trailing_output_separator(self):
        # Exact compiler and /Fd tokens from successful Windows artifact 13658789690.
        compiler = r'C:\PROGRA~1\MICROS~2\2022\ENTERP~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\cl.exe'
        self.commands[0]['command'] = (compiler +
            r' -I..\sdk\include /FdCMakeFiles\sco-plugin-check.dir\ /FS -c ..\consumer\plugin.c')
        self.check()
        self.commands[0]['command'] = self.commands[0]['command'].replace(
            r'-I..\sdk\include', r'-I..\..\Original\Engine\private')
        with self.assertRaisesRegex(ValueError, 'original source'):
            self.check()

    def test_posix_escaped_whitespace_rejected(self):
        self.commands[0]['command'] = r'cc -I../sdk/include\ space'
        with self.assertRaisesRegex(ValueError, 'unsupported POSIX command escaping'):
            self.check()

    def test_response_files_rejected(self):
        for invocation in (['cc', '@flags.rsp'], 'cl @"flags with spaces.rsp"',
                           ['cc', '-I', '@flags.rsp']):
            with self.subTest(invocation=invocation):
                self.commands[0].pop('arguments', None)
                key = 'arguments' if isinstance(invocation, list) else 'command'
                self.commands[0][key] = invocation
                with self.assertRaisesRegex(ValueError, 'response-file'):
                    self.check()

    def test_invalid_compile_commands(self):
        for commands in ([], {}, [None], [{'command': 'cl'}]):
            self.commands = commands
            with self.subTest(commands=commands), self.assertRaises(ValueError):
                self.check()

if __name__ == '__main__':
    unittest.main()
