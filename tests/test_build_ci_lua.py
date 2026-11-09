#!/usr/bin/env python3
"""Offline controls for verified-source Lua bootstrap; no download/compiler needed."""
import hashlib
from io import BytesIO
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_ci_lua as lua


def fixture():
    output = BytesIO()
    with tarfile.open(fileobj=output, mode='w:gz') as archive:
        for name in ('lua.c', 'luac.c', 'lapi.c', 'lua.h', '../escape.c'):
            item = tarfile.TarInfo(f'lua-{lua.VERSION}/src/{name}')
            item.size = 1
            archive.addfile(item, BytesIO(b'\n'))
    return output.getvalue()


class LuaBootstrap(unittest.TestCase):
    def test_bad_digest_does_not_write_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                lua.unpack(fixture(), Path(tmp))
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_build_and_runtime_check(self):
        data = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'lua with spaces'
            with patch.object(lua, 'SHA256', hashlib.sha256(data).hexdigest()), \
                 patch.object(lua, 'urlopen') as download, \
                 patch.object(lua.subprocess, 'run') as run:
                download.return_value.__enter__.return_value.read.return_value = data
                lua.build(root)
                commands = [call.args[0] for call in run.call_args_list]
                self.assertEqual(len(commands), 2)
                self.assertEqual(commands[0][0], 'cl')
                self.assertIn(str(root / 'lua.c'), commands[0])
                self.assertNotIn(str(root / 'luac.c'), commands[0])
                self.assertEqual(commands[1], [str(root / 'lua54.exe'), '-e',
                                               'assert(_VERSION == "Lua 5.4")'])
                self.assertTrue(all(call.kwargs['check'] for call in run.call_args_list))
                self.assertFalse((root.parent / 'escape.c').exists())
                with self.assertRaises(FileExistsError):
                    lua.build(root)

    def test_compile_failure_prevents_execution(self):
        data = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(lua, 'SHA256', hashlib.sha256(data).hexdigest()), \
                 patch.object(lua, 'urlopen') as download, \
                 patch.object(lua.subprocess, 'run', side_effect=lua.subprocess.CalledProcessError(1, 'cl')) as run:
                download.return_value.__enter__.return_value.read.return_value = data
                with self.assertRaises(lua.subprocess.CalledProcessError):
                    lua.build(Path(tmp) / 'build')
                self.assertEqual(run.call_count, 1)


if __name__ == '__main__':
    unittest.main()
