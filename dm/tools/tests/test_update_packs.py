"""Update packs and folder RomFS: romfs_image.py's layered trees (layered_entries, pack_layers, clone_tree), pack_dump.py's updates/<version>/ with romfs_removed.txt laid over base/romfs,
scripts/nup_update_pack.py writing such a pack from a NUP by its hashes, and the NSP script's --romfs-format. No game files needed."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs_image
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
pack_dump = nsp.pack_dump
import test_nsp_dump as dumpt   # its dump folder helpers (npdm, version)
import test_nsp_nup as nupt     # its NUP writer
spec = importlib.util.spec_from_file_location('nup_update_pack', REPO / 'scripts/nup_update_pack.py')
if spec is None or spec.loader is None: raise ImportError('scripts/nup_update_pack.py')
nup_update_pack = importlib.util.module_from_spec(spec); spec.loader.exec_module(nup_update_pack)

TID, VER = dumpt.TID, dumpt.VER


def files(root, tree):   # tree: {path: text, or None for an empty folder}
    for p, v in tree.items():
        f = Path(root) / p
        if v is None: f.mkdir(parents=True, exist_ok=True)
        else: f.parent.mkdir(parents=True, exist_ok=True); f.write_text(v)


def image_files(img):   # {guest path: bytes or None for a folder} of an image
    out = {}
    with open(img, 'rb') as f:
        for p, (d, off, size) in romfs_image.read_index(img).items():
            if d: out[p] = None
            else: f.seek(off); out[p] = f.read(size).decode()
    return out


def folder_files(root):
    out = {}
    for d, dirs, names in os.walk(root):
        for n in dirs: out[os.path.relpath(os.path.join(d, n), root)] = None
        for n in names: out[os.path.relpath(os.path.join(d, n), root)] = Path(d, n).read_text()
    return out


class Layers(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.t = Path(td.name).resolve()
        files(self.t / 'base', {'a/same': 'base', 'a/kept': 'kept', 'a/gone': 'gone', 'voice/x': 'x', 'voice/y': 'y', 'emptied/z': 'z', 'empty': None,
                                'file_or_dir': 'a base file', 'old_dir/f': 'old', 'removed_but_updated': 'base copy'})
        files(self.t / 'upd', {'a/same': 'update', 'a/new': 'new', 'file_or_dir/inside': 'the update made it a folder', 'old_dir/g': 'new in a removed folder',
                               'removed_but_updated': 'update copy'})
        self.removed = romfs_image.removed_paths(['# comment', '', '/a/gone', 'romfs/voice', 'emptied/z', 'old_dir', 'removed_but_updated', 'not/there'])
        self.want = {'a': None, 'a/same': 'update', 'a/kept': 'kept', 'a/new': 'new', 'empty': None, 'file_or_dir': None, 'file_or_dir/inside': 'the update made it a folder',
                     'old_dir': None, 'old_dir/g': 'new in a removed folder', 'removed_but_updated': 'update copy'}

    def test_removed_paths(self):
        self.assertEqual(self.removed, {'a/gone', 'voice', 'emptied/z', 'old_dir', 'removed_but_updated', 'not/there'})

    def test_merge_rules(self):
        ents = romfs_image.layered_entries([self.t / 'upd', self.t / 'base'], self.removed)
        got = {e[5]: (None if e[2] else Path(e[1]).read_text()) for e in ents[1:]}
        self.assertEqual(got, self.want)   # the update wins; removals drop base files and whole base folders, and a folder they emptied goes; an empty base folder stays

    def test_image(self):
        img = self.t / 'out/RomFS.img'; img.parent.mkdir()
        n = romfs_image.pack_layers([self.t / 'upd', self.t / 'base'], img, self.removed, reuse=True)
        self.assertEqual(n, len(self.want) + 1)
        self.assertEqual(image_files(img), self.want)
        before = img.stat().st_mtime_ns
        romfs_image.pack_layers([self.t / 'upd', self.t / 'base'], img, self.removed, reuse=True)
        self.assertEqual(img.stat().st_mtime_ns, before, 'reused while the inputs are unchanged')
        romfs_image.pack_layers([self.t / 'upd', self.t / 'base'], img, self.removed - {'a/gone'}, reuse=True)
        self.assertEqual(image_files(img)['a/gone'], 'gone', 'a new removal list packs again')

    def test_one_root_is_the_plain_packer(self):
        img = self.t / 'one.img'
        romfs_image.pack_layers([self.t / 'base'], img)
        self.assertEqual(romfs_image.read_index(img), romfs_image.read_index(self.t / 'one.img'))
        self.assertEqual(image_files(img)['a/gone'], 'gone')

    def test_folder_of_clones(self):
        dst = self.t / 'out/RomFS'
        n, cloned, copied = romfs_image.clone_tree([self.t / 'upd', self.t / 'base'], dst, self.removed, reuse=True)
        self.assertEqual((n, cloned + copied), (len(self.want) + 1, sum(v is not None for v in self.want.values())))
        self.assertEqual(folder_files(dst), self.want)
        self.assertEqual(romfs_image.clone_tree([self.t / 'upd', self.t / 'base'], dst, self.removed, reuse=True), (n, 0, 0), 'reused while the inputs are unchanged')
        (self.t / 'upd/a/same').write_text('changed')
        romfs_image.clone_tree([self.t / 'upd', self.t / 'base'], dst, self.removed, reuse=True)
        self.assertEqual((dst / 'a/same').read_text(), 'changed')
        self.assertFalse((self.t / 'out/RomFS.partial').exists())
        self.assertEqual(romfs_image.folder_format_mismatches(dst), [])
        (self.t / 'base/a/bad.zs').write_bytes(b'\0' * 8)
        romfs_image.clone_tree([self.t / 'upd', self.t / 'base'], dst, self.removed)
        self.assertEqual(romfs_image.folder_format_mismatches(dst), [('a/bad.zs', 8, b'\0' * 4)])


class Base(dumpt.Base):
    def setUp(self):
        super().setUp()
        self.pack = self.dump / f'updates/{VER}'
        (self.pack / 'romfs/dir/old').unlink() if (self.pack / 'romfs/dir/old').exists() else None
        files(self.dump / 'base/romfs', {'dir/drop': 'dropped by the update', 'dir/keep': 'kept from the base'})
        (self.pack / 'romfs_removed.txt').write_text('dir/drop\n')


class DumpPacks(Base):
    def test_scan_lists_the_pack(self):
        self.assertTrue(pack_dump.is_pack(self.pack))
        self.assertEqual(pack_dump.scan(self.dump)[1], {VER: self.pack})

    def test_laid_over_the_base(self):
        out = self.dump / 'out'
        pack_dump.pack_version(self.pack, TID, out, say=lambda s: None, base=self.dump / 'base')
        got = image_files(out / 'RomFS.img')
        self.assertEqual({k: v for k, v in got.items() if v is not None}, {'dir/new': 'data', 'dir/old': 'data', 'dir/keep': 'kept from the base'})
        self.assertEqual((out / 'ExeFS/main').read_bytes(), b'new code')

    def test_folder_format(self):
        out = self.dump / 'out'
        pack_dump.pack_version(self.pack, TID, out, say=lambda s: None, base=self.dump / 'base')
        pack_dump.pack_version(self.pack, TID, out, say=lambda s: None, base=self.dump / 'base', fmt='folder')
        self.assertFalse((out / 'RomFS.img').exists(), 'the image goes: romfs.source() would take it before the folder')
        self.assertEqual(folder_files(out / 'RomFS'), {'dir': None, 'dir/new': 'data', 'dir/old': 'data', 'dir/keep': 'kept from the base'})
        import romfs as rfs
        self.assertEqual(Path(rfs.source(out)), out / 'RomFS')
        pack_dump.pack_version(self.pack, TID, out, say=lambda s: None, base=self.dump / 'base')
        self.assertFalse((out / 'RomFS').exists())

    def test_needs_the_base_romfs(self):
        with self.assertRaisesRegex(pack_dump.Error, 'update pack'): pack_dump.pack_version(self.pack, TID, self.dump / 'out', say=lambda s: None)
        import shutil; shutil.rmtree(self.dump / 'base/romfs')
        self.assertFalse(pack_dump.is_dump(self.dump), 'a pack alone is not a whole game')

    def test_survey_lists_the_layered_files(self):
        out = self.dump.parent / 'survey'
        sys.path.insert(0, str(REPO / 'dm/games')); import profile_guess; nsp.pg = profile_guess   # main() imports it
        self.assertEqual(nsp.survey_dump(self.pack, out, self.dump / 'base'), 3)
        rows = sorted(r.split('\t')[0] for r in (out / 'romfs_manifest.tsv').read_text().splitlines()[1:])
        self.assertEqual(rows, ['/dir/keep', '/dir/new', '/dir/old'])


class Options(Base):
    def run_main(self, *opts):
        return dumpt.Main.run_main(self, *opts)

    def test_folder_needs_a_mac_build(self):
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), self.assertRaisesRegex(nsp.BuildError, 'is for a Mac build'):
            nsp.main(nsp.parse_args([str(self.dump), '--romfs-format', 'folder']))

    def test_folder_on_a_mac_build(self):
        with mock.patch.object(nsp, 'free_gb', return_value=1.0):   # a folder of clones needs no room for an image
            work, _ = self.run_main('--romfs-format', 'folder', '--platform', 'mac')
        self.assertEqual(work, self.dump / f'{TID:016X}_build')


@unittest.skipIf(nupt.zstd is None, 'no zstd module')
class NupPacks(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.t = Path(td.name).resolve()
        base = nupt.version(marker='old')
        base['romfs'].update({'keep': b'k' * 300, 'change': b'before', 'gone/a': b'x'})
        upd = nupt.version(main=b'new code', marker='old')
        upd['romfs'].update({'keep': b'k' * 300, 'change': b'after!', 'added/b': b'y' * 250})
        self.nup = self.t / 'game.nup'
        nupt.write_nup(self.nup, [(0, TID, 0, base, b''), (1, TID + 0x800, VER, upd, b'')])
        self.dump = self.t / 'dump'

    def run_pack(self, *opts):
        err = io.StringIO()
        with mock.patch.object(sys, 'argv', ['nup_update_pack.py', str(self.nup), str(self.dump), *opts]), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            try: nup_update_pack.main()
            except SystemExit as e: return str(e.code)
        return None

    def test_writes_base_and_pack(self):
        self.assertIn('no base/', self.run_pack())
        self.assertIsNone(self.run_pack('--base'))
        upd = self.dump / f'updates/{VER}'
        self.assertEqual(sorted(str(p.relative_to(upd / 'romfs')) for p in (upd / 'romfs').rglob('*') if p.is_file()), ['added/b', 'change'])
        self.assertEqual((upd / 'exefs/main').read_bytes(), b'new code')
        self.assertEqual(romfs_image.removed_paths((upd / 'romfs_removed.txt').read_text().splitlines()), {'gone/a'})
        self.assertEqual((self.dump / 'base/romfs/change').read_text(), 'before')
        out = self.t / 'out'
        pack_dump.pack_version(upd, TID, out, say=lambda s: None, base=self.dump / 'base')   # the pack laid over the base is the update's RomFS as the container holds it
        got = {k: v for k, v in image_files(out / 'RomFS.img').items() if v is not None}
        want = {k: v.decode() for k, v in nupt.version(marker='old')['romfs'].items()} | {'keep': 'k' * 300, 'change': 'after!', 'added/b': 'y' * 250}
        self.assertEqual(got, want)
        self.assertIsNone(self.run_pack(), 'a second run checks the base it finds and writes the pack again')

    def test_wrong_base_refused(self):
        self.assertIsNone(self.run_pack('--base'))
        (self.dump / 'base/romfs/change').write_text('BEFORE')   # same size, other bytes: a file the update replaces, so its hash is checked
        self.assertIn('not this container', self.run_pack())
        self.assertIsNone(self.run_pack('--no-check-base'))


if __name__ == '__main__':
    unittest.main()
