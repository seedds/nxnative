"""NSP script: scan_nsps takes a file as a game package only when its header is one nx_extract opens (PFS0 at 0, or an XCI's HEAD at 0x100), and takes every package's title ID and
version from its content metas (nx_extract --contents), never from its file name (GitHub issue #1629). The reader itself needs real keys and is not run here: its boundary is stubbed
(stand-in meta NCAs that name what a content meta would say) and its command line and output are checked. No game files or keys needed."""
import importlib.util
from pathlib import Path
import re
import shutil
import struct
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

APP = 0x010020D01AD24000


def pfs0(path, names):   # a PFS0 holding empty files with these names
    strs = b''.join(n.encode() + b'\0' for n in names); ent, o = b'', 0
    for n in names: ent += struct.pack('<QQII', 0, 0, o, 0); o += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<III', len(names), len(strs), 0) + ent + strs)


def xci(path, names):   # an XCI whose root HFS0 holds a secure partition holding empty files with these names
    def hfs0(entries):   # [(name, data bytes)]
        strs = b''.join(n.encode() + b'\0' for n, _ in entries); ent, o, so, data = b'', 0, 0, b''
        for n, d in entries:   # entry: offset, size, name, hashed size, reserved, hash
            ent += struct.pack('<QQIIQ', o, len(d), so, 0, 0) + bytes(0x20); o += len(d); so += len(n) + 1; data += d
        return b'HFS0' + struct.pack('<III', len(entries), len(strs), 0) + ent + strs + data
    head = bytearray(0x200); head[0x100:0x104] = b'HEAD'; struct.pack_into('<Q', head, 0x130, 0x200)
    path.write_bytes(bytes(head) + hfs0([('update', b''), ('secure', hfs0([(n, b'') for n in names]))]))


def meta(tid, ver=0, kind=None):   # the name of a stand-in meta NCA that says what the package's content meta says: fake_contents reads it back
    kind = kind or ('Application' if tid & 0xFFF == 0 else 'Patch' if tid & 0xFFF == 0x800 else 'AddOnContent')
    return f'{kind}-{tid:016x}-{ver}.cnmt.nca'


def fake_contents(paths):   # stands in for package_reader's contents (nx_extract --contents): the content metas its stand-in meta NCAs name
    return {p: [(k, int(t, 16), int(v)) for k, t, v in re.findall(r'(\w+)-([0-9a-f]{16})-(\d+)\.cnmt\.nca', p.read_bytes().decode('latin-1'))] for p in paths}


class Scan(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        cm = mock.patch.object(nsp, 'say', self.said.append); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_only_packages_count(self):
        base = self.tmp / f'Game [{APP:016X}][v0].xci'; xci(base, [meta(APP)])   # a cartridge dump
        upd = self.tmp / f'Update [{APP | 0x800:016X}].nsp'; pfs0(upd, [meta(APP | 0x800, 65536)])
        page = self.tmp / f'Update [{APP | 0x800:016X}] (1).nsp'; page.write_bytes(b'<!DOCTYPE html><title>404</title>')   # a failed download saved under the game's name
        short = self.tmp / f'DLC [{APP + 0x1001:016X}].xci'; short.write_bytes(b'\0' * 0x80)   # cut off before the card header
        asked = []
        app, fam = nsp.scan_nsps(self.tmp, lambda paths: asked.extend(paths) or fake_contents(paths))
        self.assertEqual(asked, [base, upd])   # nx_extract is given packages only: it would stop on the others
        self.assertEqual((app, fam['base'], fam['updates'], fam['dlc']), (APP, base, [(65536, upd)], []))
        self.assertEqual(sorted(m.split(':')[0] for m in self.said), sorted(f'skipping {p.name}' for p in (page, short)))

    def test_appledouble_skipped_quietly(self):
        base = self.tmp / f'Game [{APP:016X}][v0].nsp'; pfs0(base, [meta(APP)])
        (self.tmp / ('._' + base.name)).write_bytes(b'\0\x05\x16\x07' + b'\0' * 4092)
        self.assertEqual(nsp.scan_nsps(self.tmp, fake_contents)[1]['base'], base)
        self.assertEqual(self.said, [])

    def test_extension_in_any_letter_case(self):   # `Game [id].NSP` (a download, a card dump): glob('*.nsp') missed it on every POSIX system, a case-insensitive Mac volume too (LNPANDY/NX2Apple#554)
        base = self.tmp / f'Game [{APP:016X}][v0].NSP'; pfs0(base, [meta(APP)])
        upd = self.tmp / f'Update [{APP | 0x800:016X}][v65536].Nsp'; pfs0(upd, [meta(APP | 0x800, 65536)])
        dlc = self.tmp / f'DLC [{APP + 0x1001:016X}].XCI'; xci(dlc, [meta(APP + 0x1001)])
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual((app, fam['base'], [p for _, p in fam['updates']], [p for _, _, p in fam['dlc']]), (APP, base, [upd], [dlc]))

    def test_compressed_extension_in_capitals_counts(self):   # its conversion: test_nsp_nsz
        nsz = self.tmp / f'Game [{APP:016X}].NSZ'; pfs0(nsz, [meta(APP)])
        self.assertEqual(nsp.scan_nsps(self.tmp, fake_contents)[1]['base'], nsz)

    def test_package_beside_a_compressed_one_wins_in_any_letter_case(self):   # read from the listing: Game.nsp, which with_suffix would look for, is not on a case-sensitive volume
        for packed, compressed in (('.NSP', '.NSZ'), ('.NSP', '.nsz'), ('.nsp', '.NSZ'), ('.XCI', '.XCZ')):
            with self.subTest(packed=packed, compressed=compressed):
                self.said.clear()
                base = self.tmp / f'Game [{APP:016X}]{packed}'; pfs0(base, [meta(APP)])
                nsz = self.tmp / f'Game [{APP:016X}]{compressed}'; pfs0(nsz, [meta(APP)])
                self.assertEqual(nsp.scan_nsps(self.tmp, fake_contents)[1]['base'], base)
                self.assertEqual(self.said, [f'skipping {nsz.name}: {base.name} is beside it'])
                base.unlink(); nsz.unlink()

    def test_folder_named_like_a_package_is_not_one(self):
        (self.tmp / 'old.nsp').mkdir()
        base = self.tmp / f'Game [{APP:016X}].nsp'; pfs0(base, [meta(APP)])
        self.assertEqual(nsp.scan_nsps(self.tmp, fake_contents)[1]['base'], base)

    def test_nup_in_capitals(self):
        nup = self.tmp / 'Game.NUP'; nup.write_bytes(b'NUP\0')
        self.assertEqual(nsp.nup_file(self.tmp), nup)
        (self.tmp / '._Game.NSP').write_bytes(b'\0\x05\x16\x07')   # an AppleDouble file is not a package
        self.assertEqual(nsp.nup_file(self.tmp), nup)
        pfs0(self.tmp / 'Game.NSP', ['x.nca'])
        self.assertIsNone(nsp.nup_file(self.tmp))   # a package in capitals counts as a package, so no NUP is looked for

    def test_dump_with_a_package_in_capitals_beside_it(self):
        (self.tmp / 'base/exefs').mkdir(parents=True); (self.tmp / 'base/romfs').mkdir(); (self.tmp / 'base/exefs/main.npdm').write_bytes(b'x')
        self.assertTrue(nsp.dump_folder(self.tmp))
        pfs0(self.tmp / 'Game.NSP', ['x.nca'])
        self.assertFalse(nsp.dump_folder(self.tmp))   # the package is what the player asked for


class ContentMeta(unittest.TestCase):
    """GitHub issue #1629: the version came from the [v<N>] tag in the file name. Untagged updates beside another update were left out, a single one was taken for the newest version
    the profile registers, and a wrong tag was found only after the extraction, by the build IDs. Every package now says its title ID and version itself"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        cm = mock.patch.object(nsp, 'say', self.said.append); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_untagged_updates_however_many(self):
        base = self.tmp / 'Game.nsp'; pfs0(base, [meta(APP)])
        u1 = self.tmp / 'Game.v720896.UPD.nsp'; pfs0(u1, [meta(APP | 0x800, 720896)])   # PR #393's name
        u2 = self.tmp / 'Game update.nsp'; pfs0(u2, [meta(APP | 0x800, 786432)])
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual(nsp.nsp_versions(fam), {'v0': None, 'v720896': u1, 'v786432': u2})
        self.assertEqual(self.said, [])

    def test_a_wrong_tag_is_said_and_ignored(self):
        base = self.tmp / f'Game [{APP:016X}][v0].nsp'; pfs0(base, [meta(APP)])
        upd = self.tmp / f'Game [{APP | 0x800:016X}][v65536].nsp'; pfs0(upd, [meta(APP | 0x800, 131072)])
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual(nsp.nsp_versions(fam), {'v0': None, 'v131072': upd})
        self.assertEqual(self.said, [f'{upd.name}: its name says v65536, its content meta v131072, which is the version it is taken for'])

    def test_a_base_game_at_its_own_version(self):   # Castlevania: Belmont's Curse's base, tagged [v0], is 1.1.0 (v65536): the profile's v65536 row, not an unregistered v0
        base = self.tmp / 'Castlevania [0100BA4026E94000][v0].nsz'; pfs0(base, [meta(APP, 65536)])
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual((fam['base_version'], nsp.nsp_versions(fam)), (65536, {'v65536': None}))
        self.assertTrue(self.said[0].endswith('its name says v0, its content meta v65536, which is the version it is taken for'))
        with mock.patch.object(nsp, 'PROG', mock.Mock()):
            self.assertEqual(nsp.decompress_nsz(self.tmp, app, dict(fam, base=self.tmp / 'x.nsp'), None, {})['base_version'], 65536)   # the conversion keeps it

    def test_the_title_is_the_content_meta_s(self):   # a name's tag can be wrong (FFT's base NSP is tagged …0001), and a ticket names one title of a merged package
        base = self.tmp / f'Game [{APP | 1:016X}].nsp'; pfs0(base, [f'{APP | 0x800:016x}0000000000000011.tik', meta(APP)])
        self.assertEqual(nsp.scan_nsps(self.tmp, fake_contents)[:2], (APP, {'base': base, 'base_version': 0, 'updates': [], 'dlc': []}))

    def test_dlc_versions(self):   # extract_aoc takes the highest version of a DLC given twice
        pfs0(self.tmp / 'base.nsp', [meta(APP)])
        old, new = self.tmp / 'dlc.nsp', self.tmp / 'dlc (2).nsp'; pfs0(old, [meta(APP + 0x1001, 0)]); pfs0(new, [meta(APP + 0x1001, 65536)])
        self.assertEqual(sorted(nsp.scan_nsps(self.tmp, fake_contents)[1]['dlc']), [(APP + 0x1001, 0, old), (APP + 0x1001, 65536, new)])

    def test_unreadable_and_other_packages_are_skipped_in_red(self):   # nx_extract could not read it, or it holds no game, update or DLC: it cannot be packed
        base = self.tmp / 'base.nsp'; pfs0(base, [meta(APP)])
        broken = self.tmp / 'broken.nsp'; pfs0(broken, [])
        delta = self.tmp / 'delta.nsp'; pfs0(delta, [meta(APP | 0x800, 65536, 'Delta')])
        contents = lambda paths: fake_contents(paths) | {broken: 'the NCA header is not valid'}
        app, fam = nsp.scan_nsps(self.tmp, contents)
        self.assertEqual((fam['base'], fam['updates']), (base, []))
        self.assertEqual(sorted(self.said), [f'[RED] skipping {broken.name}: its content meta could not be read: the NCA header is not valid',f'[RED] skipping {delta.name}: no base game, update or DLC content meta (Delta)'])
        base.unlink()
        with self.assertRaisesRegex(nsp.BuildError, 'no game package with a readable content meta'): nsp.scan_nsps(self.tmp, contents)


class Reader(unittest.TestCase):
    """package_reader: one nx_extract run reads every package's content metas (`--contents <package>...`), and the keys check (`--keys`)"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.keys, self.calls, self.out, self.code = self.tmp / 'prod.keys', [], '', 0

    def fake_run(self, cmd, log, env=None, cwd=None, on_line=None, ok=()):
        self.calls.append((cmd, ok)); log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(self.out)
        if self.code and self.code not in ok: raise nsp.BuildError(f'nx_extract exit code {self.code} (full output: {log})')

    def reader(self):
        cms = [mock.patch.object(nsp, 'find_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', self.fake_run)]
        for cm in cms: cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        cm = nsp.package_reader(REPO, self.keys, {}); r = cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        return r

    def test_contents_of_every_package_in_one_run(self):
        a, b, c = self.tmp / 'a.nsp', self.tmp / 'b.nsz', self.tmp / 'c.xci'
        self.out = (f'PFS0: reading a missing final file-name terminator through an in-memory header\npackage 0 a.nsp\ncontent Application {APP:016x} 0\n'
                    f'package 1 b.nsz\ncontent Application {APP:016x} 0\ncontent Patch {APP | 0x800:016x} 1507328\ncontent AddOnContent {APP + 0x1001:016x} 65536\n'
                    'package 2 c.xci\n[RED] c.xci: no content meta\n')
        self.code = 5
        r = self.reader()
        self.assertEqual(r.contents([a, b, c]), {a: [('Application', APP, 0)],
                                                 b: [('Application', APP, 0), ('Patch', APP | 0x800, 1507328), ('AddOnContent', APP + 0x1001, 65536)],
                                                 c: 'no content meta'})
        self.assertEqual(self.calls, [(['nx', self.keys, '--contents', a, b, c], (5,))])
        self.assertEqual(r.contents([]), {}); self.assertEqual(len(self.calls), 1)

    def test_keys_that_cannot_open_the_metas_stop_with_nx_extract_s_line(self):
        self.out, self.code = ('[RED] a.nsp: prod.keys has no key_area_key_application_10 (master key 10, which came with firmware 17.0.0): the keys are older than the game; '
                               'dump prod.keys again from a console on firmware 17.0.0 or later\n'), 6
        with self.assertRaisesRegex(nsp.BuildError, r'^a\.nsp: prod\.keys has no key_area_key_application_10 .* firmware 17\.0\.0 or later$'): self.reader().contents([self.tmp / 'a.nsp'])

    def test_any_other_failure_stops_with_its_output(self):
        self.out, self.code = 'Unhandled exception. System.IO.IOException: boom\n', 134
        with self.assertRaisesRegex(nsp.BuildError, r'content metas could not be read .*\n.*System\.IO\.IOException: boom'): self.reader().contents([self.tmp / 'a.nsp'])

    def test_a_package_missing_from_the_listing_stops(self):
        self.out = 'package 0 a.nsp\n'
        with self.assertRaisesRegex(nsp.BuildError, 'listed 1 of 2 packages'): self.reader().contents([self.tmp / 'a.nsp', self.tmp / 'b.nsp'])

    def test_nx_extract_that_does_not_start_says_so(self):
        with mock.patch.object(nsp, 'NX_EXTRACT', 'dm/tools/nx_extract_bin/missing'), nsp.package_reader(REPO, self.keys, {}) as r:
            with self.assertRaisesRegex(nsp.BuildError, r'nx_extract does not start'): r.contents([self.tmp / 'Game.nsp'])

    def test_keys_too_old_stop_before_the_packages_are_read(self):   # nx_extract --keys: every package once, a merged one too; its red line becomes the stop, other failures are left to later steps
        m, upd = self.tmp / 'm.nsz', self.tmp / 'u.nsp'
        fam = {'base': m, 'updates': [(1507328, m), (1572864, upd)], 'dlc': [(APP + 0x1001, 65536, m)]}
        r = self.reader()
        self.out = 'the keys open all 2 packages\n'
        r.check_keys(fam)
        self.assertEqual(self.calls[0][0], ['nx', self.keys, '--keys', m, upd]); self.assertIn(6, self.calls[0][1])
        self.out = ('[RED] m.nsz (and 1 more files): prod.keys has no key_area_key_application_10 (master key 10, which came with firmware 17.0.0): the keys are older than the game; '
                    'dump prod.keys again from a console on firmware 17.0.0 or later\n')
        with self.assertRaisesRegex(nsp.BuildError, r'^m\.nsz \(and 1 more files\): prod\.keys has no key_area_key_application_10 .* firmware 17\.0\.0 or later$'): r.check_keys(fam)
        self.out, self.code = 'Unhandled exception. System.IO.IOException\n', 134
        r.check_keys(fam)


class Merged(unittest.TestCase):
    """A merged package: base game + update + DLC in one NSP (\"[Base eShop + Update 4.0.1 - v1507328 + 1 DLC].nsp\"), or a cartridge dump with its update. Its tickets name every
    title, the first one in any order, and nx_extract opened it as the base game alone (Mario Kart 8 Deluxe 1.0.0's main is 32-bit: the build stopped). Its content metas make it
    count as each of them"""
    CONTENTS = [('Application', APP, 0), ('Patch', APP | 0x800, 1507328), ('AddOnContent', APP + 0x1001, 65536)]

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        cm = mock.patch.object(nsp, 'say', self.said.append); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def merged_nsp(self, name='Game [Base + Update + 1 DLC].nsp'):
        p = self.tmp / name; pfs0(p, [f'{APP | 0x800:016x}{0:016x}.tik', f'{APP:016x}{0:016x}.tik', meta(APP), 'b.nca', meta(APP | 0x800, 1507328), 'd.nca', meta(APP + 0x1001, 65536)])
        return p

    def test_one_file_is_base_update_and_dlc(self):
        m = self.merged_nsp()
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual(app, APP)
        self.assertEqual(fam, {'base': m, 'base_version': 0, 'updates': [(1507328, m)], 'dlc': [(APP + 0x1001, 65536, m)]})
        self.assertEqual(nsp.nsp_versions(fam), {'v0': None, 'v1507328': m})
        self.assertTrue(any('holds several titles' in s for s in self.said))

    def test_merged_beside_separate_files(self):   # a newer update beside the merged package: both are listed, nsp_versions keeps each by its version
        m = self.merged_nsp(); upd = self.tmp / f'Update [{APP | 0x800:016X}][v1572864].nsp'; pfs0(upd, [meta(APP | 0x800, 1572864)])
        app, fam = nsp.scan_nsps(self.tmp, fake_contents)
        self.assertEqual(sorted(fam['updates']), [(1507328, m), (1572864, upd)])

    def test_extraction_estimate_counts_the_file_once(self):
        m = self.merged_nsp(); size = m.stat().st_size
        self.assertEqual(nsp.extract_bytes({'v1507328': (m, m)}, clones=False), size)

    def test_two_games_on_one_cartridge(self):   # Super Mario Galaxy + Super Mario Galaxy 2 on one XCI: two Application content metas, two games; --title-id names the one to pack
        GAME2 = 0x0100FD8022DAA000
        cart = self.tmp / 'Galaxy 1 + 2.xci'; xci(cart, [meta(APP), 'b.nca', meta(GAME2), 'd.nca'])
        with self.assertRaisesRegex(nsp.BuildError, r'more than one game .*--title-id'): nsp.scan_nsps(self.tmp, fake_contents)
        app, fam = nsp.scan_nsps(self.tmp, fake_contents, GAME2)
        self.assertEqual((app, fam), (GAME2, {'base': cart, 'base_version': 0, 'updates': [], 'dlc': []}))
        with self.assertRaisesRegex(nsp.BuildError, r'no game 0100FD8022DAB000 .* which holds'): nsp.scan_nsps(self.tmp, fake_contents, GAME2 + 0x1000)

    def test_extraction_names_the_application(self):   # nx_extract reads the scanned game's own content metas (--app), whichever package holds them
        m = self.merged_nsp(); calls = []
        def fake_run(cmd, log, **k): calls.append(cmd); (Path(cmd[4]) / 'ExeFS').mkdir(parents=True)
        with mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'swap_in'), mock.patch.object(nsp, 'extracted', return_value=None), \
             mock.patch.object(nsp, 'PROG', mock.MagicMock()):
            nsp.extract('nx', 'k', self.tmp / 'ex', 'v1507328', m, m, None, self.tmp / 'logs', mock.MagicMock(), APP)
        self.assertEqual(calls[0][calls[0].index('--app') + 1], f'{APP:016x}')

    def test_dlc_extraction_names_the_dlc(self):   # --aoc takes one DLC's data NCA by its content meta's title ID, from a merged package or a DLC NSP alike
        m = self.merged_nsp(); calls = []
        def fake_run(cmd, log, **k): calls.append(cmd); Path(cmd[4]).parent.mkdir(parents=True, exist_ok=True); Path(cmd[4]).write_bytes(b'img')
        with mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'PROG', mock.MagicMock()):
            nsp.extract_aoc('nx', 'k', REPO, self.tmp / 'work', APP, {'dlc': [(APP + 0x1001, 65536, m)]}, self.tmp / 'logs')
        self.assertEqual(calls[0][-1], f'{APP + 0x1001:016x}')

    def test_a_programs_dlc_are_the_applications(self):   # program 1 (app + 1) asks for the application's DLC by their index from the application: DLC …1001 is 1.img, not 0.img
        m = self.merged_nsp(); made = []
        def fake_run(cmd, log, **k): made.append(Path(cmd[4]).name); Path(cmd[4]).parent.mkdir(parents=True, exist_ok=True); Path(cmd[4]).write_bytes(b'img')
        with mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'PROG', mock.MagicMock()):
            nsp.extract_aoc('nx', 'k', REPO, self.tmp / 'work', APP + 1, {'dlc': [(APP + 0x1001, 0, m), (APP + 0x1002, 0, m)]}, self.tmp / 'logs')
        self.assertEqual(made, ['1.img', '2.img'])


if __name__ == '__main__':
    unittest.main()
