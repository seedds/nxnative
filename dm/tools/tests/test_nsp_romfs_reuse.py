"""NSP script RomFS image reuse across platforms (scripts/build_ipa_from_nsp.py --romfs-from, LNPANDY/NX2Apple#708): an extraction writes the image's identity beside it (title, program,
version, the packages' NCA content IDs, size, SHA-256), and a build that finds an image of this title and version left by the other platform's build checks its bytes and its files against the packages
(nx_extract --manifest) and takes it instead of extracting the RomFS again. Anything that does not match is extracted as before. nx_extract is faked; the images are real."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import nacp, profile_guess, romfs_image
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

APP = 0x0100AAAA00000000


class RomfsFrom(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='romfs-from-')).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        t = self.tmp
        self.game = t / 'game'   # what the packages hold
        for name, data in {'Data/marker': b'marker bytes', 'Data/big.bin': bytes(range(256)) * 64, 'Data/sub/small': b'x'}.items():
            (self.game / name).parent.mkdir(parents=True, exist_ok=True); (self.game / name).write_bytes(data)
        self.base, self.logs, self.ex = t / 'base.nsp', t / 'logs', t / 'extract'
        self.base.write_bytes(b'nsp'); self.ex.mkdir()
        self.calls = []
        self.packages = {'base.nsp': ['0123456789abcdef0123456789abcdef.nca', 'fedcba9876543210fedcba9876543210.cnmt.nca', 'ticket.tik']}
        for patch in (mock.patch.object(nsp, 'run', self.fake_nx), mock.patch.object(nsp, 'ROMFS_FROM', []),
                      mock.patch.object(nsp, 'package_names', lambda p: self.packages[p.name]), mock.patch.object(nsp, 'say', lambda msg: None), mock.patch.object(nsp.PROG, 'stage'), mock.patch.object(nsp.PROG, 'end')):
            patch.start(); self.addCleanup(patch.stop)

    def fake_nx(self, cmd, log, fatal=True, **kw):   # nx_extract: the code modules and control data, and the RomFS as an image or as a manifest
        mode = '--manifest' if '--manifest' in cmd else '--romfs-image'
        self.calls.append(mode)
        out = Path(cmd[4]); (out / 'ExeFS').mkdir(parents=True); (out / 'ExeFS' / 'main').write_bytes(b'NSO0')
        (out / 'control.nacp').write_bytes(b'nacp')
        if mode == '--manifest':
            rows = [f'{p}\t{n}\t{h.hex()}\n' for p, n, h in profile_guess.romfs_files(self.game)]
            (out / 'romfs_manifest.tsv').write_text(nsp.MANIFEST_HEAD + ''.join(rows))
            (out / 'romfs_nro').mkdir()
        else: romfs_image.pack(self.game, out / 'RomFS.img')
        log.parent.mkdir(parents=True, exist_ok=True); log.write_text(f'{out / "ExeFS"}: 1 files 0.0 MB\n')
        return 0

    def extract(self, name='v0', marker='Data/marker'):
        return nsp.extract(Path('nx_extract'), Path('prod.keys'), self.ex, name, self.base, None, None, self.logs, {'romfs_marker': marker}, APP)

    def donor(self, name='v0'):   # what the other platform's build left: the image with its identity file, as the launcher's games/<title ID>/romfs.img
        d = self.extract(name)
        games = self.tmp / 'games' / f'{APP:016X}'; games.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(d / 'RomFS.img', games / 'romfs.img'); shutil.copyfile(d / 'RomFS.img.id', games / 'romfs.img.id')
        shutil.rmtree(self.ex); self.ex.mkdir(); self.calls.clear()
        nsp.ROMFS_FROM[:] = [games / 'romfs.img']
        return games / 'romfs.img'

    def test_extraction_writes_the_identity(self):
        d = self.extract()
        have = json.loads((d / 'RomFS.img.id').read_text())
        self.assertEqual(have, {'title': f'{APP:016x}', 'program': 0, 'version': 'v0', 'allow_damaged': False,
                                'contents': [['0123456789abcdef0123456789abcdef', 'fedcba9876543210fedcba9876543210'], []],
                                'bytes': (d / 'RomFS.img').stat().st_size, 'sha256': nsp.image_sha256(d / 'RomFS.img')})

    def test_matching_image_is_taken_without_extracting(self):
        img = self.donor()
        d = self.extract()
        self.assertEqual(self.calls, ['--manifest'])
        self.assertEqual((d / 'RomFS.img').read_bytes(), img.read_bytes())
        self.assertEqual(json.loads((d / 'RomFS.img.id').read_text()), json.loads(Path(f'{img}.id').read_text()))
        self.assertTrue((d / 'ExeFS' / 'main').is_file() and (d / 'control.nacp').is_file())
        self.assertFalse((d / 'romfs_manifest.tsv').exists() or (d / 'romfs_nro').exists() or (self.ex / 'v0.partial').exists())
        self.assertEqual(nsp.extracted(self.logs / 'extract_v0.log')[0], 1 + 3)   # the code module and the image's files: the next build's size estimate

    def test_another_version_or_title_is_extracted(self):
        for key, value in (('version', 'v65536'), ('title', f'{APP + 1:016x}'), ('program', 1), ('allow_damaged', True)):
            with self.subTest(key=key):
                img = self.donor(); ident = Path(f'{img}.id')
                ident.write_text(json.dumps(json.loads(ident.read_text()) | {key: value}))
                self.extract()
                self.assertEqual(self.calls, ['--romfs-image'])

    def test_other_packages_of_the_version_are_extracted(self):   # a replaced dump under the same version: its NCAs have other content IDs, though the files' heads may match
        self.donor()
        self.packages['base.nsp'][0] = '1123456789abcdef0123456789abcdef.nca'
        self.extract()
        self.assertEqual(self.calls, ['--romfs-image'])

    def test_packages_that_cannot_be_listed_are_extracted(self):
        self.donor()
        with mock.patch.object(nsp, 'package_names', side_effect=nsp.BuildError('truncated PFS0 header')):
            self.extract()
        self.assertEqual(self.calls, ['--romfs-image'])

    def test_changed_bytes_are_extracted(self):   # the same size, the index whole, one byte of a file's data changed
        img = self.donor()
        data = bytearray(img.read_bytes()); i = data.index(bytes(range(256))) + 100; data[i] ^= 0xff; img.write_bytes(bytes(data))
        d = self.extract()
        self.assertEqual(self.calls, ['--romfs-image'])
        self.assertNotEqual((d / 'RomFS.img').read_bytes(), img.read_bytes())

    def test_missing_or_damaged_identity_is_extracted(self):
        for content in (None, 'not json', '[]', '{}'):
            with self.subTest(content=content):
                img = self.donor(); ident = Path(f'{img}.id')
                if content is None: ident.unlink()
                else: ident.write_text(content)
                self.extract()
                self.assertEqual(self.calls, ['--romfs-image'])

    def test_files_the_packages_do_not_hold_are_extracted(self):   # an identity and hash that match, from other game files (the packages were replaced)
        self.donor()
        (self.game / 'Data/sub/small').write_bytes(b'y')
        d = self.extract()
        self.assertEqual(self.calls, ['--manifest', '--romfs-image'])
        self.assertEqual(rows(d / 'RomFS.img'), rows(self.game))

    def test_missing_marker_is_extracted(self):
        self.donor()
        self.extract(marker='Data/none')
        self.assertEqual(self.calls, ['--manifest', '--romfs-image'])

    def test_reused_extraction_gets_no_identity(self):   # an extraction from before identity files is reused for this build, but not vouched for: it was not checked against the packages
        d = self.extract(); (d / 'RomFS.img.id').unlink(); self.calls.clear()
        self.assertEqual(self.extract(), d)
        self.assertEqual(self.calls, [])
        self.assertFalse((d / 'RomFS.img.id').exists())

    def test_option(self):
        args = nsp.parse_args(['nsps', '--romfs-from', '~/a.img', '--romfs-from', '/b/romfs.img'])
        self.assertEqual(args.romfs_from, ['~/a.img', '/b/romfs.img'])
        self.assertIsNone(nsp.parse_args(['nsps']).romfs_from)


def rows(romfs): return sorted(profile_guess.romfs_files(romfs))


if __name__ == '__main__':
    unittest.main()
