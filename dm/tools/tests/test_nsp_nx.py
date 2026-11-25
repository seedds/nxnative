"""Read-only NX input: both platforms, materialized trees and native NCA storage.
Fixtures contain artificial bytes, never game code or keys. No Xcode build runs.
"""
import hashlib
import importlib
import io
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts'))
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs_image
nsp = importlib.import_module('build_ipa_from_nsp')
pack_nx, pack_nup = nsp.pack_nx, nsp.pack_nup
FIXTURES = Path(__file__).parent / 'fixtures/nx'
APP = 0x0100000000010000


def image_file(path, name):
    isdir, offset, size = romfs_image.read_index(path)[name]
    assert not isdir
    with open(path, 'rb') as stream:
        stream.seek(offset); return stream.read(size)


class NxReader(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def open(self, name='switch1-zstd.nx'):
        n = pack_nx.Nx(FIXTURES / name); self.addCleanup(n.close); return n

    def test_both_platforms_and_storage_forms_pack_versions_and_dlc(self):
        for platform in ('switch1', 'switch2'):
            for mode in ('raw', 'zstd', 'native'):
                with self.subTest(platform=platform, mode=mode):
                    n = self.open(f'{platform}-{mode}.nx')
                    app, base, updates, dlc = pack_nup.scan(n)
                    self.assertEqual(app, APP); self.assertEqual(set(updates), {65536}); self.assertEqual(set(dlc), {APP + 0x1001})
                    out = self.root / f'{platform}-{mode}'
                    for version, package in ((0, base), (65536, updates[65536])):
                        dst = out / f'v{version}'
                        pack_nup.pack_version(n, package, base, app, dst, say=lambda _: None)
                        npdm = (dst / 'ExeFS/main.npdm').read_bytes()
                        self.assertEqual(pack_nup.program_id(npdm, 'test'), APP)
                        if mode == 'native':
                            self.assertEqual(image_file(dst / 'RomFS.img', 'data.bin'), bytes([1 if version == 0 else 2]) * 4096)
                            self.assertEqual((dst / 'ExeFS/main').read_bytes()[:4], b'NSO0')
                            self.assertEqual(len((dst / 'control.nacp').read_bytes()), 0x4000)
                            self.assertEqual((dst / 'icon.jpg').read_bytes()[:2], b'\xff\xd8')
                        else:
                            self.assertEqual((dst / 'icon.jpg').read_bytes(), bytes([0xff, 0xd8, 0xff, 0xe0, 0 if version == 0 else 1]))
                            marker = 'base.txt' if version == 0 else 'update.txt'
                            self.assertEqual(image_file(dst / 'RomFS.img', marker), b'A' * 150000)
                            self.assertEqual(image_file(dst / 'RomFS.img', 'empty'), b'')
                            self.assertIn('empty-dir', romfs_image.read_index(dst / 'RomFS.img'))
                            self.assertNotIn('update.txt' if version == 0 else 'base.txt', romfs_image.read_index(dst / 'RomFS.img'))
                    pack_nup.pack_dlc(n, app, dlc, out / 'aoc', say=lambda _: None)
                    name, data = ('dlc.bin', bytes([3]) * 8192) if mode == 'native' else ('dlc.txt', b'synthetic DLC')
                    self.assertEqual(image_file(out / 'aoc/1.img', name), data)
                    n.close()
                    self.assertIsNotNone(n.proc.poll())
                    self.assertTrue(all(tree.proc.poll() is not None for tree in n._native))

    def test_random_ranges_and_cross_block_copy(self):
        n = self.open(); _, _, versions, _ = pack_nup.scan(n)
        i = n.child(versions[65536].romfs, b'update.txt')
        self.assertIsNotNone(i)
        _, size, sha, fid = n.files[i]
        for offset, length in ((0, 32), (65530, 100), (size - 9, 9)):
            self.assertEqual(n.file_range(fid, offset, length), b'A' * length)
        output = io.BytesIO(); self.assertEqual(n.copy(i, output), size)
        self.assertEqual(hashlib.sha256(output.getvalue()).digest(), sha)

    def test_unchanged_images_and_code_are_reused(self):
        for name in ('switch1-zstd.nx', 'switch2-native.nx'):
            with self.subTest(name=name):
                n = self.open(name); app, base, versions, _ = pack_nup.scan(n)
                out = self.root / name
                self.assertEqual(pack_nup.pack_version(n, versions[65536], base, app, out, say=lambda _: None), 'packed')
                before = (out / 'ExeFS/main').stat().st_ino, (out / 'RomFS.img').stat().st_mtime_ns
                self.assertEqual(pack_nup.pack_version(n, versions[65536], base, app, out, say=lambda _: None), 'kept')
                self.assertEqual(before, ((out / 'ExeFS/main').stat().st_ino, (out / 'RomFS.img').stat().st_mtime_ns))

    def test_detection_direct_folder_and_ambiguous_containers(self):
        one = self.root / 'one'; one.mkdir()
        nx = one / 'game.nx'; shutil.copyfile(FIXTURES / 'switch1-zstd.nx', nx)
        self.assertEqual(nsp.nup_file(nx), nx); self.assertEqual(nsp.nup_file(one), nx)
        shutil.copyfile(nx, one / 'other.nx')
        with self.assertRaisesRegex(nsp.BuildError, 'more than one'): nsp.nup_file(one)
        (one / 'other.nx').unlink(); (one / 'game.nsp').write_bytes(b'package')
        self.assertIsNone(nsp.nup_file(one))

    def test_independent_header_page_recovery(self):
        data = bytearray((FIXTURES / 'switch1-zstd.nx').read_bytes())
        data[:4096] = bytes(4096)
        path = self.root / 'recovered.nx'; path.write_bytes(data)
        self.assertTrue(pack_nx.is_nx(path))
        with pack_nx.Nx(path) as n: self.assertEqual(pack_nup.scan(n)[0], APP)

    def test_corrupt_metadata_and_truncated_input_fail(self):
        source = bytearray((FIXTURES / 'switch1-zstd.nx').read_bytes())
        for label, data in (('truncated', source[:8193]), ('catalog', bytearray(source))):
            with self.subTest(label=label):
                if label == 'catalog': data[struct.unpack_from('<Q', data, 24)[0] + 16] ^= 1
                path = self.root / f'{label}.nx'; path.write_bytes(data)
                with self.assertRaises(pack_nup.Error): pack_nx.Nx(path)

    def test_corrupt_payload_is_rejected_without_publishing_image(self):
        data = bytearray((FIXTURES / 'switch1-raw.nx').read_bytes())
        # Change the first RAW byte of the large RomFS block, keeping all metadata intact.
        index = struct.unpack_from('<Q', data, 88)[0]
        count = struct.unpack_from('<Q', data, 96)[0]
        for i in range(count):
            offset, stored, logical = struct.unpack_from('<QII', data, index + i * 24)
            if logical == 65536:
                data[offset] ^= 1; break
        else: self.fail('fixture has no multi-block RomFS file')
        path = self.root / 'corrupt.nx'; path.write_bytes(data)
        with pack_nx.Nx(path) as n:
            app, base, _, _ = pack_nup.scan(n)
            out = self.root / 'bad'
            with self.assertRaises(pack_nup.Error): pack_nup.pack_version(n, base, base, app, out, say=lambda _: None)
            self.assertFalse((out / 'RomFS.img').exists())
            self.assertFalse((out / 'RomFS.img.part').exists())

    def test_wrong_application_is_rejected(self):
        with self.assertRaisesRegex(pack_nup.Error, 'matching applications'): pack_nx.Nx(FIXTURES / 'switch1-zstd.nx', APP + 1)

    def test_native_update_requires_the_exact_archived_base(self):
        for platform in ('switch1', 'switch2'):
            with self.subTest(platform=platform):
                with self.assertRaisesRegex(pack_nup.Error, 'exact native base dependency'):
                    pack_nx.Nx(FIXTURES / f'{platform}-missing-base.nx')

    def test_native_dlc_version_gate(self):
        n = self.open('switch1-native.nx'); addon = n.addons[0]
        self.assertEqual(addon.required_version, 65536)
        self.assertEqual(n.dlc_for_version(0), {})
        self.assertEqual(n.dlc_for_version(65536), {APP + 0x1001: addon})
        addon.deps = [(2, 'missing')]
        with self.assertRaisesRegex(pack_nup.Error, 'missing required dependency'): n.dlc_for_version(65536)

    def test_scan_and_extract_through_the_build_script(self):
        for name in ('switch1-zstd.nx', 'switch2-native.nx'):
            with self.subTest(name=name):
                app, fam = nsp.scan_nup(FIXTURES / name, APP)
                self.addCleanup(fam['nup'].close)
                self.assertEqual(set(nsp.nsp_versions(fam)), {'v0', 'v65536'})
                with mock.patch.object(nsp.PROG, 'stage'), mock.patch.object(nsp.PROG, 'end'), mock.patch.object(nsp.PROG, 'set'):
                    out = nsp.extract_nup(self.root / name, 'v65536', fam['updates'][0][1], fam['base'], app, fam['nup'])
                self.assertTrue((out / 'RomFS.img').is_file())
                self.assertTrue((out / 'ExeFS/main.npdm').is_file())

    def test_main_accepts_nx_without_asking_for_keys(self):
        sys.path.insert(0, str(REPO / 'dm/games'))
        game = importlib.import_module('game')
        profile = game.load('010015100B514000')
        class Stop(Exception): pass
        for name in ('switch1-zstd.nx', 'switch2-native.nx'):
            path = self.root / name; shutil.copyfile(FIXTURES / name, path)
            seen = []
            def prereqs(env, *args, seen=seen):
                seen.append(env['DM_ROOT']); raise Stop()
            with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
                 mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for keys')), mock.patch.object(nsp, 'say'), \
                 mock.patch.object(game, 'by_title', return_value=profile), mock.patch.object(nsp, 'check_prereqs', side_effect=prereqs):
                with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(path), '--platform', 'mac-app']))
            self.assertEqual(len(seen), 1)

    def test_compiler_warnings_are_errors(self):
        # The normal on-demand build uses the same source and C++17 baseline.
        source = REPO / 'dm/tools/nx_reader.cpp'
        compiler = shutil.which('clang++') or shutil.which('c++')
        if compiler is None: self.skipTest('no C++17 compiler')
        flags = []
        if shutil.which('brew'):
            prefix = subprocess.run(['brew', '--prefix', 'zstd'], capture_output=True, text=True, check=True).stdout.strip()
            flags = ['-I' + prefix + '/include']
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', *flags, str(source)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__': unittest.main()
