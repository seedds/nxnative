#!/usr/bin/env python3
import contextlib
import io
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
import sys
sys.path.insert(0, str(REPO / 'dm/mac'))
sys.path.insert(0, str(REPO / 'dm/tools'))
import pack_app
import msl_pak

class TestPackAppMslArchive(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='test_msl_archive_')).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        t = self.tmp
        for d in ('patched', 'romfs', 'msl/b-1234567890ab', 'profile', 'font', 'bin', 'sdk', 'source', 'host', 'tools', 'ios/Sources'):
            (t / d).mkdir(parents=True)
        files = {
            'dmhost': b'host',
            'patched/main.patched.bin': b'code',
            'patched/main.fixups.tsv': b'fixups',
            'romfs/marker': b'marker',
            'msl/b-1234567890ab/inputs.tsv': b'game\t010086A022444000\ninputs\t1234\n',
            'msl/b-1234567890ab/0001_b.json': b'{"chain": "1234567890ab"}',
            'msl/b-1234567890ab/0001_b.metallib': b'MTLLIB',
            'font/nintendo_ext.ttf': b'font',
            'source/launcher.c': b'launcher',
            'source/Launcher.swift': b'swift',
            'host/romfs_image.h': b'validator',
            'host/save_sync.c': b'sync',
            'host/save_sync.h': b'sync api',
            'tools/romfs.py': b'reader',
            'ios/Sources/LauncherSettings.swift': b'settings',
            'ios/Sources/LauncherRow.swift': b'row',
        }
        for n, content in files.items():
            (t / n).write_bytes(content)
        for n in pack_app.TABLES:
            (t / 'profile' / n).write_text(n)
        for tool in ('clang', 'swiftc', 'codesign'):
            p = t / 'bin' / tool
            p.write_text('fake')
            p.chmod(0o755)

        self.args = pack_app.parse([
            str(t / 'Game.app'),
            '--host', str(t / 'dmhost'),
            '--patched', str(t / 'patched'),
            '--romfs', str(t / 'romfs'),
            '--msl', str(t / 'msl/b-1234567890ab'),
            '--tables', str(t / 'profile'),
            '--font', str(t / 'font'),
            '--title-id', '010086A022444000',
            '--name', 'Test',
            '--marker', 'marker',
            '--msl-archive',
        ])

        def fake_run(cmd):
            cmd = [str(c) for c in cmd]
            name = Path(cmd[0]).name
            if name == 'otool': return 'cmd LC_BUILD_VERSION\n minos 14.0\n'
            if name == 'xcrun':
                if '--find' in cmd: return str(t / 'bin' / ('swiftc' if 'swiftc' in cmd else 'clang')) + '\n'
                if '--show-sdk-path' in cmd: return str(t / 'sdk') + '\n'
                if '--show-sdk-version' in cmd: return '27.0\n'
            if name in ('clang', 'swiftc'):
                if '--version' in cmd: return 'fixture\n'
                out = Path(cmd[cmd.index('-o') + 1])
                out.write_bytes(b'bin')
                out.chmod(0o755)
                return ''
            if name == 'codesign': return ''
            return ''

        patchers = [
            mock.patch.object(pack_app, 'run', fake_run),
            mock.patch.object(pack_app, 'HERE', t / 'source'),
            mock.patch.object(pack_app, 'can_clone', return_value=False),
            mock.patch.object(shutil, 'which', lambda n: str(t / 'bin/codesign')),
        ]
        for p in patchers:
            p.start()
            self.addCleanup(p.stop)

    def test_msl_archive_pack(self):
        with contextlib.redirect_stdout(io.StringIO()):
            pack_app.pack(self.args)

        app = Path(self.args.out)
        msl_dir = app / 'Contents/Resources/msl'
        self.assertTrue(msl_dir.is_dir())
        pak_file = msl_dir / 'b-1234567890ab.pak'
        self.assertTrue(pak_file.is_file(), "Expected b-1234567890ab.pak to exist")
        # Verify the directory does NOT exist as loose files
        self.assertFalse((msl_dir / 'b-1234567890ab').exists(), "Loose directory should not exist when --msl-archive is set")

        # Verify pak contents using msl_pak
        entries = msl_pak.list_entries(pak_file)
        names = [e[0] for e in entries]
        self.assertIn('inputs.tsv', names)
        self.assertIn('0001_b.json', names)
        self.assertIn('0001_b.metallib', names)

        # Verify Info.plist DMChain is set
        import plistlib
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual(info.get('DMChain'), '1234567890ab')

if __name__ == '__main__':
    unittest.main()
