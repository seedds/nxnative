"""NSP script with a dump folder (scripts/pack_dump.py): the folder's layout is read and packed into what the script's later steps read, and the script asks for no keys.
The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import importlib.util
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs_image
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
pack_dump = nsp.pack_dump

TID = 0x040074A01BF12000   # a game with a profile in dm/games, whose registered version is an update
VER = 458752


def npdm(tid):
    d = bytearray(0x100)
    d[:4] = b'META'
    struct.pack_into('<II', d, 0x70, 0x80, 0x20)
    d[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', d, 0x90, tid)
    return bytes(d)


def version(folder, tid=TID, main=b'code', marker='marker'):
    (folder / 'exefs').mkdir(parents=True); (folder / 'romfs/dir').mkdir(parents=True)
    (folder / 'exefs/main').write_bytes(main)
    (folder / 'exefs/main.npdm').write_bytes(npdm(tid))
    (folder / 'exefs/.listing').write_text('not a module')
    (folder / f'romfs/dir/{marker}').write_text('data')


class Base(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dump = Path(td.name).resolve() / 'dump'
        version(self.dump / 'base', marker='old')
        version(self.dump / f'updates/{VER}', main=b'new code', marker='new')
        (self.dump / 'dlc/040074a01bf13001/0/romfs').mkdir(parents=True)
        (self.dump / 'dlc/040074a01bf13001/0/romfs/dlcfile').write_text('dlc')
        (self.dump / 'dlc/.listing').write_text('not a DLC')


class Scan(Base):
    def test_layout(self):
        self.assertEqual(pack_dump.scan(self.dump), (TID, {VER: self.dump / f'updates/{VER}'}, {TID + 0x1001: self.dump / 'dlc/040074a01bf13001/0/romfs'}))

    def test_base_alone(self):
        d = self.dump.parent / 'alone'; version(d / 'base')
        self.assertEqual(pack_dump.scan(d), (TID, {}, {}))

    def test_update_romfs_alone(self):   # the update's RomFS is whole, so a dump may leave the base's out; only packing the base itself then fails
        d = self.dump.parent / 'slim'; version(d / 'base'); version(d / f'updates/{VER}', main=b'new code', marker='new')
        shutil.rmtree(d / 'base/romfs')
        self.assertTrue(pack_dump.is_dump(d))
        self.assertEqual(pack_dump.scan(d), (TID, {VER: d / f'updates/{VER}'}, {}))
        with self.assertRaisesRegex(pack_dump.Error, 'no exefs/main'): pack_dump.pack_version(d / 'base', TID, d / 'out', say=lambda s: None)
        shutil.rmtree(d / f'updates/{VER}/romfs')
        self.assertFalse(pack_dump.is_dump(d))

    def test_update_alone(self):   # an update's folder is the whole game, so a dump may have no base/ (Fire Emblem Fortune's Weave)
        d = self.dump.parent / 'update'; version(d / f'updates/{VER}')
        self.assertTrue(nsp.dump_folder(d))
        self.assertEqual(pack_dump.scan(d), (TID, {VER: d / f'updates/{VER}'}, {}))
        app, fam = nsp.scan_dump(d)
        self.assertIsNone(fam['base'])
        self.assertEqual(nsp.nsp_versions(fam), {f'v{VER}': d / f'updates/{VER}'})

    def test_title_id_of_the_newest_whole_update(self):
        d = self.dump.parent / 'updates2'; version(d / f'updates/{VER}'); (d / f'updates/{VER + 1}/romfs').mkdir(parents=True)
        self.assertEqual(pack_dump.scan(d)[0], TID)

    def test_flat_folder_is_the_base_game(self):   # exefs/ and romfs/ directly in the folder, as nup extract writes them (issue #1265)
        d = self.dump.parent / 'flat'; version(d)
        self.assertTrue(nsp.dump_folder(d))
        self.assertEqual(pack_dump.scan(d), (TID, {}, {}))
        app, fam = nsp.scan_dump(d)
        self.assertEqual((app, fam['base']), (TID, d))
        self.assertEqual(nsp.nsp_versions(fam), {'v0': None})
        (d / 'base/exefs').mkdir(parents=True); (d / 'base/romfs').mkdir(); (d / 'base/exefs/main.npdm').write_bytes(npdm(TID + 0x2000))
        self.assertEqual(pack_dump.scan(d)[0], TID + 0x2000)   # base/ wins

    def test_refuses(self):
        (self.dump / 'dlc/0100000000000000/romfs').mkdir(parents=True)
        with self.assertRaisesRegex(pack_dump.Error, 'is not a DLC of'): pack_dump.scan(self.dump)
        d = self.dump.parent / 'patch'; version(d / 'base', tid=TID + 0x800)
        with self.assertRaisesRegex(pack_dump.Error, 'not a base title ID'): pack_dump.scan(d)
        with self.assertRaisesRegex(pack_dump.Error, 'no base/exefs'): pack_dump.scan(self.dump.parent)

    def test_npdm_bounds_and_header_aci0_text(self):
        p = self.dump / 'test.npdm'
        with self.assertRaises(FileNotFoundError): pack_dump.program_id(p)
        # Earlier ACI0 in header text is ignored in favor of the header-declared ACI0 section
        d = bytearray(npdm(TID)); d[0x20:0x24] = b'ACI0'; d[0x30:0x38] = b'1.0.0\0\0\0'
        p.write_bytes(bytes(d))
        self.assertEqual(pack_dump.program_id(p), TID)
        # Truncated META header and out-of-bounds ACI0 raise pack_dump.Error, never struct.error
        for raw, msg in ((b'META', 'not an NPDM'),
                         (b'META' + bytes(0x70), 'not an NPDM'),
                         (d[:0x90], 'no ACI0 section')):
            p.write_bytes(bytes(raw))
            with self.assertRaisesRegex(pack_dump.Error, msg): pack_dump.program_id(p)
        bad = bytearray(d); struct.pack_into('<II', bad, 0x70, 0x20, 0x20)
        p.write_bytes(bytes(bad))
        with self.assertRaisesRegex(pack_dump.Error, 'no ACI0 section'): pack_dump.program_id(p)
        bad = bytearray(d); struct.pack_into('<II', bad, 0x70, 0x80, 0x200)
        p.write_bytes(bytes(bad))
        with self.assertRaisesRegex(pack_dump.Error, 'no ACI0 section'): pack_dump.program_id(p)

    def test_packages_win_over_the_layout(self):
        self.assertTrue(nsp.dump_folder(self.dump))
        (self.dump / 'game.nsp').write_bytes(b'x')
        self.assertFalse(nsp.dump_folder(self.dump))

    def test_appledouble_is_no_package(self):
        (self.dump / '._game.nsp').write_bytes(b'x')
        self.assertTrue(nsp.dump_folder(self.dump))


class Pack(Base):
    def out(self): return self.dump / 'out'

    def test_update_packed_on_its_own(self):
        pack_dump.pack_version(self.dump / f'updates/{VER}', TID, self.out(), say=lambda s: None)
        self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'new code')
        self.assertFalse((self.out() / 'ExeFS/.listing').exists())
        names = romfs_image.read_index(self.out() / 'RomFS.img')
        self.assertIn('dir/new', names)
        self.assertNotIn('dir/old', names)   # an update's RomFS is the patched tree, never laid over the base

    def test_other_title_refused(self):
        with self.assertRaisesRegex(pack_dump.Error, 'the base is'): pack_dump.pack_version(self.dump / f'updates/{VER}', TID + 0x2000, self.out(), say=lambda s: None)

    def test_repeat_keeps_what_is_unchanged(self):
        src = self.dump / f'updates/{VER}'
        pack_dump.pack_version(src, TID, self.out(), say=lambda s: None)
        main, img = self.out() / 'ExeFS/main', self.out() / 'RomFS.img'
        before = main.stat().st_ino, img.stat().st_mtime_ns
        pack_dump.pack_version(src, TID, self.out(), say=lambda s: None)
        self.assertEqual((main.stat().st_ino, img.stat().st_mtime_ns), before)
        (src / 'exefs/main').write_bytes(b'patched code')
        pack_dump.pack_version(src, TID, self.out(), say=lambda s: None)
        self.assertEqual(main.read_bytes(), b'patched code')

    def test_dlc_images_follow_the_folder(self):
        aoc = self.out() / 'aoc'
        aoc.mkdir(parents=True); (aoc / '7.img').write_bytes(b'gone'); (aoc / '7.img.inputs.json').write_text('{}')
        _, _, dlc = pack_dump.scan(self.dump)
        pack_dump.pack_dlc(TID, dlc, aoc, say=lambda s: None)
        self.assertEqual(sorted(p.name for p in aoc.iterdir()), ['1.img', '1.img.inputs.json'])
        self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))


class Stop(Exception): pass


class Main(Base):
    def run_main(self, *opts):
        args = nsp.parse_args([str(self.dump), *opts])
        seen, said = {}, []
        def stop(env, *a): seen.update(env); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=stop), mock.patch.object(nsp, 'say', side_effect=said.append), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(args)
        return Path(seen['DM_ROOT']), said

    def test_needs_no_keys_and_builds_beside_the_dump(self):
        work, said = self.run_main()
        self.assertEqual(work, self.dump / f'{TID:016X}_build')
        self.assertTrue(any(s.startswith('a dump folder: ') for s in said), said)

    def test_icon_is_checked_before_the_build(self):   # a dump has no control data, so --icon / ICON is its only icon: a wrong path stops at once, not after the extraction
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=AssertionError('went on')), self.assertRaisesRegex(nsp.BuildError, r'no such icon file \(--icon / ICON\)'):
            nsp.main(nsp.parse_args([str(self.dump), '--icon', str(self.dump / 'missing.png')]))
        (self.dump.parent / 'icon.png').write_bytes(b'png')
        self.run_main('--icon', str(self.dump.parent / 'icon.png'))
        with mock.patch.object(nsp, 'ICON', str(self.dump / 'missing.jpg')), self.assertRaisesRegex(nsp.BuildError, 'no such icon file'): self.run_main()

    def test_fallback_icon_never_replaces_the_games_own(self):   # --fallback-icon / FALLBACK_ICON: a front end's stand-in, passed without knowing whether the game carries an icon
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=AssertionError('went on')), self.assertRaisesRegex(nsp.BuildError, r'no such icon file \(--fallback-icon / FALLBACK_ICON\)'):
            nsp.main(nsp.parse_args([str(self.dump), '--fallback-icon', str(self.dump / 'missing.png')]))
        with mock.patch.object(nsp, 'FALLBACK_ICON', str(self.dump / 'missing.jpg')), self.assertRaisesRegex(nsp.BuildError, 'no such icon file'): self.run_main()
        given, spare = self.dump.parent / 'given.png', self.dump.parent / 'spare.png'; given.write_bytes(b'png'); spare.write_bytes(b'png')
        self.run_main('--fallback-icon', str(spare))
        game, base = self.dump.parent / 'icons/v', self.dump.parent / 'icons/base'; game.mkdir(parents=True); base.mkdir()
        self.assertEqual(nsp.app_icon(None, None, game, base), (None, False))
        self.assertEqual(nsp.app_icon(None, spare, game, base), (spare, True))        # a dump, a NUP without an icon: the stand-in
        self.assertEqual(nsp.app_icon(given, spare, game, base), (given, False))      # --icon is asked for by name: it wins over both
        (base / 'icon.jpg').write_bytes(b'base')
        self.assertEqual(nsp.app_icon(None, spare, game, base), (base / 'icon.jpg', False))   # the game's own icon stays
        (game / 'icon.jpg').write_bytes(b'game')
        self.assertEqual(nsp.app_icon(None, spare, game, base), (game / 'icon.jpg', False))   # the packed version's before the base game's
        self.assertEqual(nsp.app_icon(given, None, game, base), (given, False))

    def test_kept_icon_reads_existing_mac_app_bundle(self):
        work = self.dump.parent / 'work'; work.mkdir()
        app = self.dump.parent / 'Game.app'
        self.assertIsNone(nsp.kept_icon(app, work))   # no app yet
        (app / 'Contents/Resources').mkdir(parents=True)
        plist = app / 'Contents/Info.plist'
        plist.write_bytes(b'<plist><dict><key>broken')   # malformed XML raises ExpatError, not ValueError
        self.assertIsNone(nsp.kept_icon(app, work))
        plist.write_bytes(plistlib.dumps(['not', 'a', 'dict']))
        self.assertIsNone(nsp.kept_icon(app, work))
        for bad in (123, '', '../escape.icns'):
            plist.write_bytes(plistlib.dumps({'CFBundleIconFile': bad}))
            self.assertIsNone(nsp.kept_icon(app, work))
        plist.write_bytes(plistlib.dumps({'CFBundleIconFile': 'AppIcon'}))
        self.assertIsNone(nsp.kept_icon(app, work))   # missing .icns
        icns = app / 'Contents/Resources/AppIcon.icns'; icns.write_bytes(b'icns')
        calls = []
        def fake_sips(cmd, **kw):
            calls.append(cmd)
            out = Path(cmd[cmd.index('--out') + 1])
            out.write_bytes(b'png-from-' + Path(cmd[4]).read_bytes())
            return subprocess.CompletedProcess(cmd, 0)
        with mock.patch.object(nsp.subprocess, 'run', side_effect=fake_sips):
            got = nsp.kept_icon(app, work)
        self.assertEqual(got, work / 'kept-icon.png')
        self.assertEqual((work / 'kept-icon.png').read_bytes(), b'png-from-icns')
        self.assertEqual(calls[0][:5], ['sips', '-s', 'format', 'png', str(icns)])
        # Explicit .icns extension in CFBundleIconFile
        custom = app / 'Contents/Resources/Custom.icns'; custom.write_bytes(b'custom')
        plist.write_bytes(plistlib.dumps({'CFBundleIconFile': 'Custom.icns'}))
        with mock.patch.object(nsp.subprocess, 'run', side_effect=fake_sips):
            got = nsp.kept_icon(app, work)
        self.assertEqual(got, work / 'kept-icon.png')
        self.assertEqual((work / 'kept-icon.png').read_bytes(), b'png-from-custom')
        # Failed sips must not reuse a stale kept-icon.png from an earlier run
        with mock.patch.object(nsp.subprocess, 'run', return_value=subprocess.CompletedProcess(['sips'], 1)):
            self.assertIsNone(nsp.kept_icon(app, work))
        self.assertFalse((work / 'kept-icon.png').exists())
        with mock.patch.object(nsp.subprocess, 'run', side_effect=OSError('no sips')):
            self.assertIsNone(nsp.kept_icon(app, work))

    def test_mac_app_rebuild_in_place_icon_selection(self):
        out_app = self.dump.parent / 'Game.app'
        (out_app / 'Contents/Resources').mkdir(parents=True)
        (out_app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIconFile': 'AppIcon'}))
        (out_app / 'Contents/Resources/AppIcon.icns').write_bytes(b'old-icns')
        explicit = self.dump.parent / 'new.png'; explicit.write_bytes(b'new')
        named_kept = self.dump.parent / 'kept-icon.png'; named_kept.write_bytes(b'spare')

        def run_to_say_game(*opts, game_icon=False, in_programs=None):
            args = nsp.parse_args([str(self.dump), '--platform', 'mac-app', '-o', str(out_app), *opts])
            if in_programs is not None: args.in_programs = in_programs
            said, game_events, kept_calls = [], [], []
            def fake_extract(ex, ver, src, app):
                d = ex / ver; (d / 'ExeFS').mkdir(parents=True, exist_ok=True); (d / 'RomFS').mkdir(exist_ok=True)
                if game_icon: (d / 'icon.jpg').write_bytes(b'game-jpg')
                else: (d / 'icon.jpg').unlink(missing_ok=True)
                return d
            def fake_kept(app_path, work):
                kept_calls.append(app_path)
                out = work / 'kept-icon.png'; out.write_bytes(b'kept-png'); return out
            def fake_say_game(app, name, version, bundle_id, out, icon):
                game_events.append((out, icon))
            src_stub = {'graphics': ('vulkan', 'Vulkan'), 'glslc_capture': (False, ''), 'vk_capture': (False, ''),
                        'gl_capture': (False, ''), 'gl_fixed': (False, ''), 'ue_decode': (False, ''),
                        'oodle_packs': (False, ''), 'ue_legacy': (False, '')}
            with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), \
                 mock.patch.object(nsp, 'PROD_KEYS', ''), mock.patch.object(nsp, 'ICON', ''), mock.patch.object(nsp, 'FALLBACK_ICON', ''), \
                 mock.patch.object(nsp, 'apple_silicon', return_value=True), mock.patch.object(nsp, 'check_prereqs'), \
                 mock.patch.object(nsp, 'extract_dump', side_effect=fake_extract), mock.patch.object(nsp, 'check_formats'), \
                 mock.patch.object(nsp, 'check_versions_aarch64'), mock.patch.object(nsp, 'build_mismatch', return_value=[]), \
                 mock.patch.object(nsp, 'build_sources', return_value=src_stub), mock.patch.object(nsp, 'extract_aoc', return_value=None), \
                 mock.patch.object(nsp, 'kept_icon', side_effect=fake_kept), mock.patch.object(nsp, 'say_game', side_effect=fake_say_game), \
                 mock.patch.object(nsp, 'say', side_effect=said.append), mock.patch.object(nsp, 'rewrite', side_effect=Stop()), \
                 mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked')):
                with self.assertRaises(Stop): nsp.main(args)
            return game_events, said, kept_calls

        # Terminal rebuild in place with no icon option and no game icon keeps the existing app's icon
        events, said, kept_calls = run_to_say_game()
        self.assertEqual(kept_calls, [out_app])
        self.assertEqual(events[0][1].name, 'kept-icon.png')
        self.assertTrue(any('keeps the icon it has (Game.app)' in s for s in said), said)

        # Rebuild that chooses a different explicit --icon overrides the existing app's icon
        events, said, kept_calls = run_to_say_game('--icon', str(explicit))
        self.assertEqual(kept_calls, [])
        self.assertEqual(events[0][1], explicit)
        self.assertFalse(any('keeps the icon it has' in s for s in said), said)

        # Rebuild with explicit --fallback-icon overrides the existing app's icon, even if named kept-icon.png
        events, said, kept_calls = run_to_say_game('--fallback-icon', str(named_kept))
        self.assertEqual(kept_calls, [])
        self.assertEqual(events[0][1], named_kept)
        self.assertTrue(any('takes the fallback icon' in s for s in said), said)
        self.assertFalse(any('keeps the icon it has' in s for s in said), said)

        # A game with its own extracted icon.jpg uses it instead of the old app's icon
        events, said, kept_calls = run_to_say_game(game_icon=True)
        self.assertEqual(kept_calls, [])
        self.assertEqual(events[0][1].name, 'icon.jpg')

        # Secondary program call in a multi-program build does not extract kept_icon
        events, said, kept_calls = run_to_say_game(in_programs=True)
        self.assertEqual(kept_calls, [])
        self.assertEqual(events, [])

    def test_title_id_must_match(self):
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), self.assertRaisesRegex(nsp.BuildError, 'which holds'):
            nsp.main(nsp.parse_args([str(self.dump), '--title-id', '0100000000010000']))

    def surveyed(self, d):   # the real survey on a dump with no profile: this NPDM is a 32-bit program's, which stops it right after the code modules and the manifest
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_capstone'), mock.patch.object(nsp, 'say'), \
             mock.patch.object(nsp, 'find_nx_extract', side_effect=AssertionError('built nx_extract')), mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), \
             self.assertRaisesRegex(nsp.BuildError, rf'{d.name} \(0400000000000000\) is a 32-bit'):
            nsp.main(nsp.parse_args([str(d)]))
        self.assertTrue((d / '0400000000000000_survey/extract/ExeFS/main').is_file())

    def test_no_profile_surveyed(self):   # it used to stop: the survey read NSPs only. The folder names the game, as a dump has no control data
        d = self.dump.parent / 'unknown'; version(d / 'base', tid=0x0400000000000000); self.surveyed(d)

    def test_update_alone_surveyed(self):
        d = self.dump.parent / 'update'; version(d / f'updates/{VER}', tid=0x0400000000000000); self.surveyed(d)

    def test_flat_folder_surveyed(self):
        d = self.dump.parent / 'flat'; version(d, tid=0x0400000000000000); self.surveyed(d)


class Survey(Base):
    def test_files_as_the_manifest_pass_leaves_them(self):
        src = self.dump.parent / 'v'; version(src, marker='a.bin')
        nro = b'\0' * 16 + b'NRO0' + b'x' * 300
        (src / 'romfs/nro').mkdir(); (src / 'romfs/nro/m.NRO').write_bytes(nro)
        out = self.dump.parent / 'out'
        self.assertEqual(nsp.survey_dump(src, out), 2)
        self.assertEqual((out / 'ExeFS/main').read_bytes(), b'code')
        self.assertFalse((out / 'ExeFS/.listing').exists())
        rows = (out / 'romfs_manifest.tsv').read_text().splitlines()
        self.assertEqual(rows[0], nsp.MANIFEST_HEAD.rstrip('\n'))
        self.assertEqual(sorted(rows[1:]), sorted([f'/dir/a.bin\t4\t{b"data".hex()}', f'/nro/m.NRO\t{len(nro)}\t{nro[:32].hex()}']))
        self.assertEqual((out / 'romfs_nro/nro/m.NRO').read_bytes(), nro)

    def test_a_version_without_code_or_romfs_stops(self):
        src = self.dump.parent / 'v'; (src / 'romfs').mkdir(parents=True)
        with self.assertRaisesRegex(nsp.BuildError, 'no exefs/main.npdm and romfs'): nsp.survey_dump(src, self.dump.parent / 'out')

    def test_same_build_reads_main_from_the_folders(self):   # a known build under another title ID: the newest update's main, then the base game's; an update with no code is passed over
        def nso(bid): return b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50)
        d = self.dump.parent / 'alias'; version(d / 'base', main=nso('ab' * 16)); version(d / f'updates/{VER}', main=nso('cd' * 16))
        app, fam = nsp.scan_dump(d)
        seen, prof = [], mock.Mock(id='P', builds=lambda: {'v0': {'main': 'ab' * 16}})
        games = mock.Mock(by_build=lambda bid, skip=(): seen.append(bid) or (prof if bid == 'ab' * 16 else None))
        with mock.patch.dict(sys.modules, {'game': games}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'find_nx_extract', side_effect=AssertionError('built nx_extract')):
            self.assertEqual(nsp.same_build(d, app, fam, None, REPO, {}), (prof, 'v0', 'ab' * 16))
            self.assertEqual(seen, ['cd' * 16, 'ab' * 16])
            shutil.rmtree(d / f'updates/{VER}/exefs'); seen.clear()
            self.assertEqual(nsp.same_build(d, app, fam, None, REPO, {}), (prof, 'v0', 'ab' * 16))
            self.assertEqual(seen, ['ab' * 16])


if __name__ == '__main__':
    unittest.main()
