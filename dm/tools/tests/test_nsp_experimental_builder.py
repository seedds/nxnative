"""NSP script, the experimental builder (scripts/build_ipa_from_nsp.py, DM_EXPERIMENTAL_BUILDER and NXB_BIN): which nxbuild binary a stage takes, and that a stage it does not
support, fails or gets wrong runs the standard way. The nxbuild binaries are stand-in scripts in a temporary folder: no game files, keys or nxbuild needed."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
romfs_image = importlib.util.module_from_spec(spec); spec.loader.exec_module(romfs_image)

APP = 0x010015100B514000


def pfs0(path, names):   # a PFS0 (NSP) with empty files of these names
    strs = b''.join(n.encode() + b'\0' for n in names)
    ent, off = b'', 0
    for n in names: ent += struct.pack('<QQII', 0, 0, off, 0); off += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<III', len(names), len(strs), 0) + ent + strs)
    return path


def tool(folder, name, body):   # an executable stand-in for an nxbuild binary: Python, run with this interpreter
    p = folder / name
    p.write_text(f'#!{sys.executable}\nimport os, shutil, sys\nfrom pathlib import Path\n{body}\n')
    p.chmod(0o755)
    return p


class Builder(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: nsp.rmtree(self.t))
        self.bin = self.t / 'bin'; self.bin.mkdir()
        self.nxb = tool(self.bin, 'nxb', 'sys.exit(0)')
        self.logs = self.t / 'logs'
        self.said = []
        for cm in (mock.patch.object(nsp.PROG, 'say', side_effect=self.said.append), mock.patch.object(nsp, '_nxb_noted', set()),
                   mock.patch.dict(os.environ, {'DM_EXPERIMENTAL_BUILDER': '1', 'NXB_BIN': str(self.nxb)})):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def text(self): return '\n'.join(self.said)

    def test_off_by_default_and_with_another_value(self):
        for value in (None, '0', 'yes'):
            env = {k: v for k, v in os.environ.items() if k != 'DM_EXPERIMENTAL_BUILDER'} | ({'DM_EXPERIMENTAL_BUILDER': value} if value else {})
            with self.subTest(value=value): self.assertIsNone(nsp.nxb_tool('extract', env))
        self.assertEqual(self.said, [], 'off says nothing')

    def test_a_stage_binary_beside_nxb_else_its_subcommand(self):
        self.assertEqual(nsp.nxb_tool('split'), [self.nxb, 'split'], 'one binary: the stage is its subcommand')
        split = tool(self.bin, 'nxb-split', 'sys.exit(0)')
        self.assertEqual(nsp.nxb_tool('split'), [split], 'a stage binary of its own goes first')
        self.assertEqual(nsp.nxb_tool('split', dict(os.environ, NXB_BIN=str(self.bin))), [split], 'NXB_BIN may name the folder')
        self.assertEqual(self.said, [])
        self.assertIsNone(nsp.nxb_tool('split', dict(os.environ, NXB_BIN=str(self.t / 'gone'))))
        self.assertIn('NXB_BIN does not name the nxb binary', self.text())
        empty = self.t / 'empty'; empty.mkdir()
        self.assertIsNone(nsp.nxb_tool('split', dict(os.environ, NXB_BIN=str(empty))))
        self.assertIn('the standard builder runs every stage', self.text())
        self.said.clear(); nsp.nxb_tool('split', dict(os.environ, NXB_BIN=str(empty))); self.assertEqual(self.said, [], 'a note once per build')

    def test_a_subcommand_nxb_does_not_have_falls_back(self):   # the one-binary nxb of a release without the stage: its usage error, exit 2
        tool(self.bin, 'nxb', "print('nxb: extract: not implemented yet', file=sys.stderr); sys.exit(2)")
        tmp = self.t / 'v0.partial'
        self.assertFalse(nsp.nxb_extract(self.t / 'k', pfs0(self.t / 'g.nsp', ['a.cnmt.nca']), None, tmp, self.logs, 'v0', APP, None))
        self.assertIn('exit code 2', self.text())
        self.assertIn('the standard builder runs it', self.text())

    def extraction(self, body, marker='marker.txt'):
        src = self.t / 'src'; src.mkdir(exist_ok=True); (src / 'marker.txt').write_text('x')
        romfs_image.pack(src, self.t / 'image.img')
        tool(self.bin, 'nxb-extract', body.replace('IMAGE', repr(str(self.t / 'image.img'))))
        base = pfs0(self.t / 'Game [010015100B514000].nsp', ['a.nca', 'a.cnmt.nca'])
        tmp = self.t / 'extract' / 'v0.partial'; tmp.parent.mkdir(exist_ok=True)
        return nsp.nxb_extract(self.t / 'prod.keys', base, None, tmp, self.logs, 'v0', APP, marker), tmp

    WRITES = ("out = Path(sys.argv[sys.argv.index('--out') + 1]); (out / 'ExeFS').mkdir(parents=True); (out / 'ExeFS/main').write_bytes(b'NSO0')\n"
              "shutil.copyfile(IMAGE, out / 'RomFS.img'); print('title {title}'); print('ExeFS: 4 files 1.0 MB, 0 identical to the compare dir (cloned), 1.0 MB newly written')")

    def test_extract_takes_a_whole_output_of_the_game(self):
        ok, tmp = self.extraction(self.WRITES.format(title=f'{APP:016x}'))
        self.assertTrue(ok, self.text())
        self.assertTrue((tmp / 'RomFS.img').is_file())
        self.assertEqual(nsp.extracted(self.logs / 'extract_v0.log'), (4, 2**20), 'the summary nx_extract writes, for the next estimate')
        self.assertIn('extracted v0 with nxb-extract', self.text())

    def test_extract_falls_back_on_unsupported_failure_or_a_bad_output(self):
        for body, why in (('sys.exit(4)', 'not supported yet'), ("print('[RED] not supported yet: sparse NCA'); sys.exit(1)", 'not supported yet'),
                          ('sys.exit(3)', 'exit code 3'), (self.WRITES.format(title='0100000000001000'), 'failed the checks'),
                          (self.WRITES.format(title=f'{APP:016x}').replace("print('title", "Path(out / 'RomFS.img').unlink(); print('title"), 'failed the checks')):
            with self.subTest(why=why, body=body[:30]):
                self.said.clear(); nsp._nxb_noted.clear()
                ok, tmp = self.extraction(body)
                self.assertFalse(ok)
                self.assertFalse(tmp.exists(), 'nx_extract starts from an empty folder')
                self.assertIn(why, self.text())
                self.assertIn('the standard builder runs it', self.text())
                self.assertNotIn('[RED]', self.text(), 'nxbuild\'s red lines stay in its log: the build did not fail')

    def test_extract_leaves_packages_it_does_not_read_to_nx_extract(self):
        tool(self.bin, 'nxb-extract', f"Path({str(self.t / 'ran')!r}).write_text('x')")
        tmp = self.t / 'v0.partial'
        for names in (['a.cnmt.nca', 'b.cnmt.nca'], ['a.ncz', 'a.cnmt.nca']):   # merged, NSZ
            with self.subTest(names=names):
                base = pfs0(self.t / 'g.nsp', names)
                self.assertFalse(nsp.nxb_extract(self.t / 'k', base, None, tmp, self.logs, 'v0', APP, None))
        xci = self.t / 'g.xci'; xci.write_bytes(b'\0' * 0x100 + b'HEAD')
        self.assertFalse(nsp.nxb_extract(self.t / 'k', xci, None, tmp, self.logs, 'v0', APP, None))
        with mock.patch.object(nsp, 'PROGRAM_INDEX', 1):
            self.assertFalse(nsp.nxb_extract(self.t / 'k', pfs0(self.t / 'g.nsp', ['a.cnmt.nca']), None, tmp, self.logs, 'v0', APP + 1, None))
        self.assertIn('not supported yet', self.text())
        self.assertFalse((self.t / 'ran').exists(), 'nxb-extract is not run for them')

    def test_an_update_is_extracted_alone(self):
        tool(self.bin, 'nxb-extract', f"Path({str(self.t / 'args')!r}).write_text(' '.join(sys.argv[1:])); sys.exit(4)")
        base, upd = pfs0(self.t / 'b.nsp', ['a.cnmt.nca']), pfs0(self.t / 'u.nsp', ['u.cnmt.nca'])
        tmp = self.t / 'v1.partial'
        self.assertFalse(nsp.nxb_extract(self.t / 'k', base, upd, tmp, self.logs, 'v1', APP, None))
        args = (self.t / 'args').read_text()
        self.assertIn(f'--update {upd} {tmp} --updates-only', args)
        self.assertFalse(tmp.with_name('v1.partial.nxb-base').exists(), 'the base\'s scratch folder goes')

    def nup(self, body, kind=None, version=0, marker='marker.txt'):   # a NUP (stand-in: the path and the version's package) through nxb-extract --nup
        src = self.t / 'src'; src.mkdir(exist_ok=True); (src / 'marker.txt').write_text('x')
        romfs_image.pack(src, self.t / 'image.img')
        if body: tool(self.bin, 'nxb-extract', body.replace('IMAGE', repr(str(self.t / 'image.img'))))   # None: the stand-in stays as it is
        path = self.t / 'game.nup'
        if not path.exists(): path.write_bytes(b'NUP\1')
        ex = self.t / 'work' / 'extract'; ex.mkdir(parents=True, exist_ok=True)
        k = SimpleNamespace(kind=nsp.pack_nup.UPDATE if version else nsp.pack_nup.BASE, version=version)
        return nsp.nxb_extract_nup(ex, f'v{version}', k, None, APP, SimpleNamespace(path=path), marker), ex / f'v{version}'

    NUP_WRITES = ("a = sys.argv; ver = a[a.index('--version') + 1]; out = Path(a[a.index('--work') + 1]) / 'extract' / ('v' + ver); (out / 'ExeFS').mkdir(parents=True); (out / 'ExeFS/main').write_bytes(b'NSO0')\n"
                  "shutil.copyfile(IMAGE, out / 'RomFS.img'); aoc = Path(a[a.index('--work') + 1]) / 'aoc'; aoc.mkdir(); (aoc / '1.img').write_bytes(b'dlc')\n"
                  "print('{app} version ' + ver + ': game.nup (update ' + ver + ')')")

    def test_a_nup_is_handed_over_with_its_version_and_no_discarded_image(self):
        ok, dst = self.nup(self.NUP_WRITES.format(app=f'{APP:016X}') + f"\nPath({str(self.t / 'args')!r}).write_text(' '.join(sys.argv[1:]))", version=131072)
        self.assertTrue(ok, self.text())
        args = (self.t / 'args').read_text()
        self.assertIn(f'--nup {self.t / "game.nup"} --work {self.t / "work" / "nxb_nup_v131072.partial"} --version 131072', args)
        self.assertNotIn('--discard-images', args)
        self.assertTrue((dst / 'RomFS.img').is_file() and (dst / 'ExeFS' / 'main').is_file())
        self.assertTrue((self.t / 'work' / 'aoc' / '1.img').is_file(), 'the DLC images come from the same run')
        self.assertFalse((self.t / 'work' / 'nxb_nup_v131072.partial').exists())
        self.assertIn('extracted v131072 with nxb-extract', self.text())

    def test_a_nup_base_is_version_zero_and_an_unchanged_nup_is_reused(self):
        body = self.NUP_WRITES.format(app=f'{APP:016X}') + f"\nPath({str(self.t / 'runs')!r}).open('a').write('x')"
        ok, dst = self.nup(body)
        self.assertTrue(ok, self.text())
        self.assertTrue(ok and (dst / 'RomFS.img').is_file())
        self.said.clear()
        ok, _ = self.nup(None)
        self.assertTrue(ok)
        self.assertEqual((self.t / 'runs').read_text(), 'x', 'the second call did not run nxb-extract again')
        self.assertIn('reusing it', self.text())

    def test_a_nup_falls_back_on_unsupported_failure_or_a_bad_output(self):
        for body, why in (('sys.exit(4)', 'not supported yet'), ('sys.exit(3)', 'exit code 3'), (self.NUP_WRITES.format(app='0100000000001000'), 'failed the checks'),
                          (self.NUP_WRITES.format(app=f'{APP:016X}').replace("(out / 'ExeFS/main').write_bytes(b'NSO0')", 'pass'), 'failed the checks')):
            with self.subTest(why=why, body=body[:30]):
                self.said.clear(); nsp._nxb_noted.clear()
                ok, dst = self.nup(body)
                self.assertFalse(ok)
                self.assertFalse(dst.exists(), 'pack_nup starts from what it finds: nothing of nxb-extract stays')
                self.assertFalse((self.t / 'work' / 'nxb_nup_v0.partial').exists())
                self.assertIn(why, self.text())
                self.assertIn('the standard builder runs it', self.text())

    def test_a_nup_without_the_builder_runs_the_standard_way(self):
        with mock.patch.dict(os.environ, {'DM_EXPERIMENTAL_BUILDER': '0'}):
            ok, dst = self.nup(self.NUP_WRITES.format(app=f'{APP:016X}'))
        self.assertFalse(ok)
        self.assertFalse(dst.exists())

    def split(self, rows):
        tool(self.bin, 'nxb-split', "a = sys.argv; o = lambda k: Path(a[a.index(k) + 1])\n"
             "o('--pkg').mkdir(parents=True)\n"
             f"for k in ('--map', '--facts', '--pairs', '--pso-pairs'): o(k).write_text({rows!r})")
        game = self.t / 'game'; game.mkdir(exist_ok=True); (game / 'game.tsv').write_text('name\tx\n')
        for n in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv'): (game / n).write_text('# header of the profile\nrow\t1\n')
        g = mock.Mock(dir=game, table=lambda n: game / n)
        d = self.t / 'extract' / 'v0'
        tmp = self.t / 'pkg.partial'; (tmp / 'tables').mkdir(parents=True, exist_ok=True)
        return nsp.nxb_split([d / 'RomFS.img'], [d / 'ExeFS' / 'main'], tmp, ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv'), self.logs, g), tmp

    def test_split_is_taken_when_its_tables_are_the_profiles(self):
        text, tmp = self.split('# another header\nrow\t1\n')
        self.assertEqual(text, 'shaders: 1 (nxb-split)\n')
        self.assertTrue((tmp / 'pkg').is_dir())

    def test_split_with_other_tables_runs_the_standard_way(self):
        text, tmp = self.split('row\t2\n')
        self.assertIsNone(text)
        self.assertEqual(sorted(p.name for p in tmp.iterdir()), ['tables'], 'shader_split.py starts from an empty folder')
        self.assertEqual(list((tmp / 'tables').iterdir()), [])
        self.assertIn('differs from the profile\'s tables', self.text())


if __name__ == '__main__':
    unittest.main()
