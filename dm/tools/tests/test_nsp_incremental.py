"""NSP script reuse across derived and provisional builds. Real fingerprints and mock tool runs; no game files, keys or Xcode needed."""
import collections
from functools import partial
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain, game, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

TID = 0x0100AAAA00000000
BID, BID2 = 'ab' * 16, 'cd' * 16


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


def control(path, disp):   # control.nacp with the display version at 0x3060
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(bytes(0x3060) + disp.encode().ljust(0x10, b'\0') + bytes(0xa0))


class ProvisionalVersionSelection(unittest.TestCase):
    def test_stale_base_profile_never_silently_ignores_update(self):
        with self.assertRaisesRegex(nsp.BuildError, "profile-out.*Refusing to silently"):
            nsp.provisional_version({"v0": None, "v983040": None}, {"v0": {}}, "", Path("cached-profile"))

    def test_explicit_base_selection_is_preserved(self):
        self.assertEqual(nsp.provisional_version({"v0": None, "v983040": None}, {"v0": {}}, "v0", Path("cached-profile")), "v0")

    def test_selected_update_must_be_available(self):
        with self.assertRaisesRegex(nsp.BuildError, "not in the input folder"):
            nsp.provisional_version({"v0": None}, {"v0": {}, "v983040": {}}, "v983040", Path("cached-profile"))

    def test_current_profile_uses_numeric_newest_input(self):
        self.assertEqual(nsp.provisional_version({"v0": None, "v9": None, "v10": None}, {"v0": {}, "v9": {}, "v10": {}}, "", Path("cached-profile")), "v10")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        self.src = self.tmp / 'games' / f'{TID:016X}'; self.src.mkdir(parents=True)
        (self.src / 'game.tsv').write_text('\n'.join([f'title_id\t{TID:016X}', f'program_id\t{TID:016X}', 'name\tTest Game', 'modules\tmain', 'app_category\tpublic.app-category.games',
                                                      'romfs_marker\ta/b', f'build\tv0\t1.0.0\tmain={BID}', 'package\tv0', 'shader_versions\tv0']) + '\n')
        (self.src / 'shader_map.tsv').write_text('# registered\npkg.bin\t0\tvertex\tregistered\tx\n')
        (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 1}\n')
        (self.src / 'pso_manifest.tsv').write_text('S\tk\tvertex\n')
        (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "old"\n')
        self.ex = self.tmp / 'extract/v65536'; nso(self.ex / 'ExeFS/main', BID2); control(self.ex / 'control.nacp', '1.1.0')
        (self.ex / 'RomFS').mkdir(); (self.ex / 'RomFS/a.bin').write_bytes(b'romfs')
        self.pdir = self.tmp / 'derived' / f'{TID:016X}'
        self.imported = {}   # what main imports from modules the list leaves out (profile_guess.main_modules, with its own tests: these NSOs are headers only)
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None),
                   mock.patch.object(nsp.pg, 'main_modules', lambda exefs, mods: self.imported)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def derive(self, ver='v65536', mods=('main',), app=None):
        return nsp.derive_profile(game.Game(self.src), self.pdir, ver, self.ex, list(mods), app)

    def redump(self, path):   # identical bytes and size, but a fresh extraction or capture timestamp
        before = path.stat()
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns + 1_000_000_000))


class DeriveProfile(Base):
    def later_stages(self):   # what later stages (a check boot, the shader split) write into the derived profile
        nsp.set_keys(game.Game(self.pdir), {'mem_used': ('0x4800000', 'rewritten modules')}, 'After the rewrite')
        (self.pdir / 'shader_map.tsv').write_text('# split\npkg.bin\t0\tvertex\tderived\tx\n')

    def assert_kept(self):
        self.assertEqual(game.Game(self.pdir).get('mem_used'), '0x4800000')
        self.assertIn('derived', (self.pdir / 'shader_map.tsv').read_text())

    def assert_fresh(self):
        self.assertNotIn('mem_used', [k for k, _ in game.Game(self.pdir).rows])
        self.assertFalse((self.pdir / 'shader_map.tsv').exists())

    def test_cross_title_marker_uses_actual_regional_resources(self):
        import romfs_image
        for image in (False, True):
            for present in (True, False):
                with self.subTest(image=image, original_marker_present=present):
                    rom = self.ex / 'RomFS'
                    shutil.rmtree(rom, ignore_errors=True); rom.mkdir()
                    (self.ex / 'RomFS.img').unlink(missing_ok=True)
                    (rom / 'CPK').mkdir()
                    (rom / 'CPK/ALL_ASIA.CPK').write_bytes(b'fixture' * 10)
                    if present: (rom / 'CPK/ALL_USEU.CPK').write_bytes(b'fixture')
                    text = (self.src / 'game.tsv').read_text().replace('romfs_marker\ta/b', 'romfs_marker\tCPK/ALL_USEU.CPK')
                    (self.src / 'game.tsv').write_text(text)
                    if image:
                        romfs_image.pack(str(rom), str(self.ex / 'RomFS.img'))
                        shutil.rmtree(rom)
                    self.derive(app=TID + 0x1000)
                    marker = 'CPK/ALL_USEU.CPK' if present else 'CPK/ALL_ASIA.CPK'
                    self.assertEqual(game.Game(self.pdir).get('romfs_marker'), marker)
                    self.assertTrue((nsp.rfs.source(self.ex) / marker).is_file())
                    self.later_stages(); self.derive(app=TID + 0x1000); self.assert_kept()
                    self.assertEqual(game.Game(self.src).get('romfs_marker'), 'CPK/ALL_USEU.CPK')

    def test_same_title_keeps_missing_marker_for_validation(self):
        self.derive(app=TID)
        self.assertEqual(game.Game(self.pdir).get('romfs_marker'), 'a/b')

    def test_cross_title_empty_romfs_keeps_missing_marker_for_validation(self):
        (self.ex / 'RomFS/a.bin').unlink()
        self.derive(app=TID + 0x1000)
        self.assertEqual(game.Game(self.pdir).get('romfs_marker'), 'a/b')

    def test_shader_only_profile_keeps_missing_marker(self):
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], app=TID + 0x1000, shader_only=True)
        self.assertEqual(game.Game(self.pdir).get('romfs_marker'), 'a/b')

    def test_unchanged_inputs_keep_later_stages(self):
        self.assertEqual(self.derive(), '1.1.0')
        self.later_stages()
        self.assertEqual(self.derive(), '1.1.0')
        self.assert_kept()
        self.assertEqual(game.Game(self.pdir).builds()['v65536']['main'], BID2)
        self.assertEqual(self.derive(), '1.1.0'); self.assert_kept()   # still kept on a third run

    def test_a_module_the_version_does_not_ship_is_left_out(self):   # Mario Kart World 1.0.0 has no subsdk1, which the 1.8.0 profile loads
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain subsdk1 sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID)
        self.derive(mods=('main', 'subsdk1', 'sdk'))
        g = game.Game(self.pdir)
        self.assertEqual(g.words('modules'), ['main', 'sdk'])
        self.assertIn('# this version ships no subsdk1', (self.pdir / 'game.tsv').read_text())
        self.assertNotIn('subsdk1', g.builds()['v65536'])
        nso(self.ex / 'ExeFS/subsdk1', BID2); self.derive(mods=('main', 'subsdk1', 'sdk'))   # a version that ships it keeps the profile's list
        self.assertEqual(game.Game(self.pdir).words('modules'), ['main', 'subsdk1', 'sdk'])

    def test_a_module_main_imports_that_the_list_leaves_out_is_added(self):   # Absolum: subsdk2 was the movie module in 1.1.2; 1.2.1 ships it as subsdk0, and no subsdk2 (issue #1211)
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain subsdk2 sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID); nso(self.ex / 'ExeFS/subsdk0', 'ee' * 16); nso(self.ex / 'ExeFS/subsdk1', 'ff' * 16)
        self.imported = {'subsdk0': 'NintendoSDK_movie'}
        self.derive(mods=('main', 'subsdk2', 'sdk'))
        g = game.Game(self.pdir); text = (self.pdir / 'game.tsv').read_text()
        self.assertEqual(g.words('modules'), ['main', 'subsdk0', 'sdk'])
        self.assertEqual(g.build_modules()[BID2], ['main', 'subsdk0', 'sdk'])   # the host loads what the row names; subsdk1 stays out
        self.assertIn('# this version ships no subsdk2', text)
        self.assertIn('# main imports from subsdk0 (NintendoSDK_movie), which the profile does not list', text)
        self.assertEqual(g.builds()['v65536']['subsdk0'], 'ee' * 16)
        self.assertNotIn('subsdk1', g.builds()['v65536'])

    def test_nothing_imported_keeps_the_list(self):
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID); nso(self.ex / 'ExeFS/subsdk0', 'ee' * 16)
        self.derive(mods=('main', 'sdk'))
        g = game.Game(self.pdir)
        self.assertEqual(g.words('modules'), ['main', 'sdk'])
        self.assertNotIn('# main imports', (self.pdir / 'game.tsv').read_text())
        self.assertNotIn('subsdk0', g.builds()['v65536'])

    def test_shader_tables_alone_add_no_module(self):
        self.imported = {'subsdk0': 'NintendoSDK_movie'}
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual(game.Game(self.pdir).words('modules'), ['main'])

    def test_a_profile_is_never_derived_onto_its_own_folder(self):
        own = self.tmp / 'profile' / f'{TID:016X}'; shutil.copytree(self.src, own)   # this script's own profile, where its derived copy would go
        before = {p.name: p.read_bytes() for p in own.iterdir()}
        with self.assertRaises(nsp.BuildError):   # before: the folder's files were deleted, then copied from themselves (Chained Echoes: FileNotFoundError, its tables gone)
            nsp.derive_profile(game.Game(own), own, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual({p.name: p.read_bytes() for p in own.iterdir()}, before)

    def test_the_same_profile_in_another_checkout_keeps_later_stages(self):
        self.derive(); self.later_stages()
        other = self.tmp / 'checkout2' / 'games' / f'{TID:016X}'; shutil.copytree(self.src, other)   # the repository cloned again, or a front end's own copy of it
        nsp.derive_profile(game.Game(other), self.pdir, 'v65536', self.ex, ['main'])
        self.assert_kept()
        self.assertNotIn(str(self.tmp), (self.pdir / 'game.tsv').read_text())

    def test_changed_inputs_derive_again(self):
        changes = {
            'source game.tsv': lambda: (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'pad_layout\tfull\n'),
            'source manual table': lambda: (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 2}\n'),
            'main build': lambda: nso(self.ex / 'ExeFS/main', 'ef' * 16),
            'display version': lambda: control(self.ex / 'control.nacp', '1.2.0'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                change(); self.derive(); self.assert_fresh()
                for p in self.src.iterdir():
                    if p.name not in nsp.DERIVED_SKIP: self.assertEqual((self.pdir / p.name).read_bytes(), p.read_bytes())
        self.assertEqual(game.Game(self.pdir).builds()['v65536'], {'display': '1.2.0', 'main': 'ef' * 16})

    def test_changed_arguments_derive_again(self):
        nso(self.ex / 'ExeFS/sdk', BID)   # in the build row only when the modules name it
        for what, kw in (('version', {'ver': 'v131072'}), ('modules', {'mods': ('main', 'sdk')}), ('title', {'app': 0x0100AAAA00FFF000})):
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                self.derive(**kw); self.assert_fresh()
        self.assertEqual(game.Game(self.pdir).title_id, 0x0100AAAA00FFF000)

    def test_a_file_the_split_does_not_read_refreshes_the_copies_and_keeps_later_stages(self):   # a geo_mesh.tsv row used to wipe the split's tables
        changes = {
            'source chain header': lambda: (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "new"\n'),
            'new source file': lambda: (self.src / 'bf_want.tsv').write_text('# new\n'),
            'edited source file': lambda: (self.src / 'pso_manifest.tsv').write_text('S\tk3\tpixel\n'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                change(); self.derive(); self.assert_kept()
                for p in self.src.iterdir():
                    if p.name not in nsp.DERIVED_SKIP: self.assertEqual((self.pdir / p.name).read_bytes(), p.read_bytes())
                self.assertEqual(self.derive(), '1.1.0'); self.assert_kept()   # and the next run reuses the folder as it is

    def test_removed_source_file_goes(self):
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        (self.src / 'pso_manifest.tsv').unlink(); self.derive()
        self.assertFalse((self.pdir / 'pso_manifest.tsv').exists()); self.assert_kept()

    def test_no_split_key_derives_again(self):   # a folder derived before the split key, or one whose split key is gone
        self.derive(); self.later_stages()
        nsp.derived_split_key(self.pdir).unlink()
        (self.src / 'pso_manifest.tsv').write_text('S\tk3\tpixel\n'); self.derive(); self.assert_fresh()

    def test_edited_copy_is_kept_until_inputs_change(self):   # the stored digest is of the inputs, not of the derived files
        self.derive(); (self.pdir / 'pso_manifest.tsv').write_text('S\tk2\tpixel\n'); self.derive()
        self.assertEqual((self.pdir / 'pso_manifest.tsv').read_text(), 'S\tk2\tpixel\n')

    def test_shader_only_derivation_preserves_builds_and_registered_patch_file(self):
        source = self.src / 'game.tsv'
        source.write_text(source.read_text() + f'build\tv65536\t1.1.0\tmain={BID2}\nshader_versions\tv0 v65536\n')
        patch = self.src / 'patches.py'; patch.write_text(f'CUTS = {{"{BID}": []}}\n')
        g = game.Game(self.src)
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        derived = game.Game(self.pdir)
        self.assertEqual(derived.rows, g.rows)
        self.assertEqual(derived.builds(), g.builds())
        self.assertTrue((self.pdir / 'patches.py').is_symlink())
        self.assertEqual((self.pdir / 'patches.py').resolve(), patch.resolve())
        derived.table('shader_map.tsv').write_text('generated DLC shader rows\n')
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual(derived.table('shader_map.tsv').read_text(), 'generated DLC shader rows\n')

    def test_derived_main_without_frame_step_stubs_keeps_the_games_step(self):
        source = self.src / 'game.tsv'; source.write_text(source.read_text() + 'fps_target\t60\n')
        (self.src / 'patches.py').write_text(f'def frame_step(ctx): pass\nFRAME_STEP = {{"{BID}": frame_step}}\n')   # stubs for the registered main only, never run
        self.derive()
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '0')
        self.assertIn('frame step stubs for other builds only', (self.pdir / 'game.tsv').read_text())
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1, "{BID2}": 2}}\n')   # the derived main has stubs too
        self.derive(); self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text('CUTS = {}\n'); self.derive()   # no frame step in the profile
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1}}\n'); self.derive()
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], shader_only=True)   # shader tables only: the settings stay
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1}}\nMATCH_FRAME_STEP = {{"main": []}}\n'); self.derive()   # stubs placed by function may serve it
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')

    def test_missing_corrupt_and_duplicate_override_rows_derive_again(self):
        for change in ('missing', 'corrupt', 'override', 'indented override', 'truncated'):
            with self.subTest(change):
                self.derive(); p = self.pdir / 'game.tsv'; original = p.read_text()
                if change == 'missing': broken = original.replace('romfs_marker\ta/b\n', '')
                elif change == 'corrupt': broken = original.replace('romfs_marker\ta/b', 'romfs_marker\tx/y')
                elif change == 'override': broken = 'romfs_marker\tx/y\n' + original
                elif change == 'indented override': broken = ' romfs_marker\tx/y\n' + original
                else: broken = original[:original.index('romfs_marker')]
                p.write_text(broken)
                self.derive()
                self.assertEqual(p.read_text(), original)

    def test_repeated_source_rows_and_computed_additions_are_kept(self):
        p = self.src / 'game.tsv'; p.write_text(p.read_text() + 'extra\tone\nextra\tone\n')
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        p = self.pdir / 'game.tsv'; p.write_text(p.read_text().replace('extra\tone\n', '', 1))
        self.derive()
        self.assertEqual(game.Game(self.pdir).values('extra'), [['one'], ['one']])

    def test_derived_replace_failure_preserves_original(self):
        self.derive(); p = self.pdir / 'game.tsv'; before = p.read_bytes()
        with mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'): self.derive(ver='v131072')
        self.assertEqual(p.read_bytes(), before)

    def test_set_keys_replace_failure_preserves_original(self):
        self.derive(); g = game.Game(self.pdir); before = g.table('game.tsv').read_bytes()
        with mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'):
                nsp.set_keys(g, {'mem_used': ('0x4800000', 'rewritten modules')}, 'After the rewrite')
        self.assertEqual(g.table('game.tsv').read_bytes(), before)
        self.assertEqual(sorted(p.name for p in self.pdir.iterdir() if p.name.startswith('.game.tsv')), [])

    def test_heap_stamp_replace_failure_preserves_original(self):
        self.derive(); nsp.set_keys(game.Game(self.pdir), {'mem_alias': ('0x80000000', '')}, 'A profile'); g = game.Game(self.pdir); before = g.table('game.tsv').read_bytes()
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(*args): (logs / 'heap_check.log').write_text('>>> nnMain\n'); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), \
             mock.patch.object(nsp.pg, 'heap_signals', return_value={}), mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'):
                nsp.heap_check(REPO, self.ex, self.tmp / 'patched', self.tmp / 'work', {}, logs, g)
        self.assertEqual(g.table('game.tsv').read_bytes(), before)

    def test_heap_check_runs_on_the_profiles_alias_region(self):   # its keys are for that layout, the iPhone's, not the Mac host's 64 GB
        self.derive(); nsp.set_keys(game.Game(self.pdir), {'mem_alias': ('0x80000000', '')}, 'A profile'); g = game.Game(self.pdir)
        logs = self.tmp / 'logs'; logs.mkdir(); seen = {}
        def boot(host, patched, henv, *args): seen.update(henv); (logs / 'heap_check.log').write_text('>>> nnMain\n'); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), mock.patch.object(nsp.pg, 'heap_signals', return_value={}):
            nsp.heap_check(REPO, self.ex, self.tmp / 'patched', self.tmp / 'work', {}, logs, g)
        self.assertEqual(seen['DM_ALIAS_SIZE'], '0x80000000')

    def test_heap_check_boots_again_after_it_resizes_the_heap_region(self):   # the app pool check behind the heap request runs only on the second boot
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x80000000\nmem_reserve\t0x140000000\nmem_stack\t0x10000000\napp_pool\t0xc8000000\n')
        self.derive(); logs = self.tmp / 'logs'; logs.mkdir(); aliases = []
        runs = iter(['>>> nnMain\n[dm] SetHeapSize OOM: req 0xc9000000 > cap 0x8000000\n', '>>> nnMain\n[mem] SetHeapSize 0xc9000000 > app pool 0xc8000000 less used memory 0x4000000\n'])
        def boot(host, patched, henv, *args): aliases.append(henv['DM_ALIAS_SIZE']); (logs / 'heap_check.log').write_text(next(runs)); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
            g = nsp.heap_check(REPO, self.ex, self.tmp / 'work/patched_v65536', self.tmp / 'work', {}, logs, game.Game(self.pdir))
        self.assertEqual(aliases, ['0x80000000', '0x50000000'])   # the second boot runs on the resized layout, and there is no third
        self.assertEqual((g.get('mem_alias'), g.values('mem_used')), ('0x50000000', []))   # the host charges the used memory it reports: no module size to make up for
        self.assertIn('the heap request did not fit, then the heap did not fit the app pool less used memory; set mem_alias', g.table('game.tsv').read_text())

    def check_boot(self, name, log):   # a check on the derived profile, whose boot writes this log; the profile after it
        logs = self.tmp / 'logs'; logs.mkdir(exist_ok=True); patched = self.tmp / 'work/patched_v65536'
        def boot(*args): (logs / f'{name}_check.log').write_text(log); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
            if name == 'heap': return nsp.heap_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
            return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))

    def test_a_check_boot_sets_the_depth_mode_the_game_calls(self):   # before the prebuild reads it (Super Mario RPG: 0 of 34 lookups hit an archive made at the default mode)
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x80000000\nmem_reserve\t0x140000000\nmem_stack\t0x10000000\napp_pool\t0x40000000\n')
        for name in ('heap', 'region'):
            with self.subTest(check=name):
                self.derive()
                self.assertEqual(self.check_boot(name, '>>> nnMain\n').get('depth_mode', 'none'), 'none')   # no call: unknown, not GL
                g = self.check_boot(name, '>>> nnMain\n[dm] [nvn] depth mode near=0 (as D3D / Metal)\n')
                self.assertEqual(g.get('depth_mode'), 'zero')
                self.assertEqual(g.get('mem_alias'), '0x80000000')   # the keys around it stay
                g = self.check_boot(name, '>>> nnMain\n')   # a later boot that stops earlier keeps what an earlier one saw
                self.assertEqual(g.get('depth_mode'), 'zero')
                self.assertEqual([k for k, _ in g.rows].count('depth_mode'), 1)
                (self.pdir / 'game.tsv').unlink()   # the next check derives afresh

    def test_depth_mode_is_no_shader_input(self):   # a check that sets it must not schedule the game's shaders again
        self.derive(); before = dmsh_chain.inputs(game.Game(self.pdir))
        g = nsp.set_keys(game.Game(self.pdir), {'depth_mode': ('zero', 'seen')}, 'Address region check')
        self.assertEqual(dmsh_chain.inputs(g), before)

    def test_a_provisional_profile_moves_to_dm_games_with_its_shader_inputs(self):   # Super Mario RPG: "(provisional profile)" in the note scheduled all 82,032 libraries again for identical shaders
        F = {'romfs': {'dirs': collections.Counter(), 'extb': {}, 'exts': {}, 'extc': {}, 'biggest': (0, 'big.bin')}, 'sdk': '14', 'program': None, 'mods': {'main': {'bid': BID}}}
        pdir = self.tmp / 'provisional' / f'{TID:016X}'
        with mock.patch.object(nsp.pg, 'guess', return_value={'modules': ('main', '')}):
            nsp.write_profile(pdir, TID, {'F': F, 'name': 'Some Game', 'survey': 'survey.md', 'version': 'v0', 'display': '1.0.0'})
        g = game.Game(pdir)
        self.assertEqual(g.get('shader_note'), 'Some Game shaders')
        self.assertIn('Provisional game profile', g.table('game.tsv').read_text())   # the file still says what it is, in a comment
        moved = self.tmp / 'moved' / f'{TID:016X}'; shutil.copytree(pdir, moved)
        text = [l for l in (moved / 'game.tsv').read_text().splitlines() if not l.startswith('#')]   # a registered profile's own comments
        (moved / 'game.tsv').write_text('\n'.join(text) + '\n')
        self.assertEqual(dmsh_chain.inputs(game.Game(moved)), dmsh_chain.inputs(g))

    def test_region_stamp_follows_the_rewrite(self):   # a derived build is checked once per rewrite: a new cut checks again, a reused derivation keeps the stamp
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x80000000\n')   # every profile has one (the host is built with it)
        self.derive(); patched = self.tmp / 'work/patched_v65536'
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(host, patched_dir, henv, *args):
            self.assertNotIn('DM_ALIAS_SIZE', henv)
            self.assertEqual(henv.get('DM_KEEP_ALIAS_CUTS'), '1')
            (logs / 'region_check.log').write_text('>>> nnMain\n'); (logs / 'regions.tsv').write_text('# table\n'); return 0
        def check(key):
            patched.with_name(patched.name + '.key').parent.mkdir(parents=True, exist_ok=True); patched.with_name(patched.name + '.key').write_text(key + '\n')
            with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
                return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {'DM_ALIAS_SIZE': '0x118000000'}, logs, game.Game(self.pdir))
        g = check('a' * 64)
        self.assertTrue(nsp.region_checked(g, patched))
        self.derive(); self.assertTrue(nsp.region_checked(game.Game(self.pdir), patched))   # the derivation's inputs are unchanged: its stamp stays
        patched.with_name(patched.name + '.key').write_text('b' * 64 + '\n')
        self.assertFalse(nsp.region_checked(game.Game(self.pdir), patched))
        g = check('b' * 64)
        stamps = [l for l in g.table('game.tsv').read_text().splitlines() if l.startswith('# address regions: ')]
        self.assertEqual(len(stamps), 1); self.assertTrue(stamps[0].endswith(f'(rewrite {"b" * 12})'))
        self.assertNotIn('\n\n\n', g.table('game.tsv').read_text())

    def test_a_mac_only_stamp_does_not_answer_for_an_ipa(self):   # issue #2860: the Mac-only pack ends ok, early exits report why, and the next build with an IPA still boots the game and reports the regions in red
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x20000000\n')
        self.derive(); patched = self.tmp / 'work/patched_v65536'; key = patched.with_name(patched.name + '.key')
        key.parent.mkdir(parents=True); key.write_text('a' * 64 + '\n')
        logs = self.tmp / 'logs'; logs.mkdir()
        table = ['# alias region 0x1000000000; offset\tsize\twho\tmapped\tfirst opened\tend\tvia\n' + f'{0x7000000:#x}\t{37 << 30:#x}\tmain+0x185a8e0\t{0x14800000:#x}\t0\t{0x920000000:#x}\t0\n']
        boots = []
        exits: list[int | None] = [None]   # boot_mac: None, still running when the window ended; otherwise the exit code
        log_text = ['>>> nnMain\n']
        def boot(*args): boots.append(1); (logs / 'region_check.log').write_text(log_text[0]); (logs / 'regions.tsv').write_text(table[0]); return exits[0]
        def check(mac_only):   # main's rule: the check runs unless this build's stamp is there. (what it said, whether the step ended ok), None when it did not run
            g = game.Game(self.pdir)
            if nsp.region_checked(g, patched, mac_only): return None
            said, end = [], mock.Mock()
            with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), mock.patch.object(nsp, 'say', said.append), mock.patch.object(nsp.PROG, 'end', end):
                nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, g, mac_only)
            return said, end.call_args.kwargs['ok']
        def ran(mac_only):   # a check that must run
            result = check(mac_only); self.assertIsNotNone(result); assert result is not None
            return result
        def stamp(): return next(l for l in (self.pdir / 'game.tsv').read_text().splitlines() if l.startswith('# address regions: '))
        def clear_stamp(): (self.pdir / 'game.tsv').write_text(''.join(l for l in (self.pdir / 'game.tsv').read_text().splitlines(True) if not l.startswith('# address regions: ')))

        # Early exit or timeout during the check: a refused request leaves no row, so Mac-only stays red with the explicit stop reason and a Mac-only stamp
        for rc, text, why in ((1, '>>> nnMain\n', 'the host stopped before the end of the check window (exit 1)'),
                              (1, '', 'the host stopped before nnMain (exit 1)'),
                              (None, '', 'the host never reached nnMain within the time limit')):
            with self.subTest(rc=rc, nnmain=bool(text)):
                clear_stamp(); exits[0] = rc; log_text[0] = text
                said, ok = ran(True)
                self.assertFalse(ok)
                self.assertTrue(any(m.startswith('[RED] address regions:') and why in m for m in said), said)
                self.assertFalse(any('fixed offsets' in m for m in said), said)
                self.assertTrue(stamp().endswith(f'(rewrite {"a" * 12}, Mac only)'))

        clear_stamp(); boots.clear(); exits[0] = None; log_text[0] = '>>> nnMain\n'
        said, ok = ran(True)   # a Mac app that ran through its window: no failure, and no red line
        self.assertTrue(ok); self.assertFalse([m for m in said if '[RED]' in m])
        self.assertTrue(any("within this Mac-only build's 64.00 GB alias region" in m for m in said), said)
        self.assertTrue(stamp().endswith(f'(rewrite {"a" * 12}, Mac only)'))
        self.assertIsNone(check(True)); self.assertEqual(len(boots), 1)   # the next Mac-only build skips it
        said, ok = ran(False)   # the same rewrite with an IPA: boots again and is red, as before
        self.assertEqual(len(boots), 2); self.assertFalse(ok)
        self.assertTrue(any(m.startswith('[RED] address regions:') and 'the iPhone layout' in m for m in said), said)
        self.assertFalse(any('Mac-only' in m for m in said))
        self.assertTrue(stamp().endswith(f'(rewrite {"a" * 12})'))
        self.assertEqual(sum(l.startswith('# address regions: ') for l in (self.pdir / 'game.tsv').read_text().splitlines()), 1)
        self.assertIsNone(check(False)); self.assertIsNone(check(True)); self.assertEqual(len(boots), 2)   # its verdict answers for either build

        key.write_text('b' * 64 + '\n'); table[0] = table[0].split('\n')[0] + '\n0x4000000\t0x1000000\tmain+0x10\t0\t-\t-\t0\n'   # a new rewrite whose regions fit the iPhone too
        said, ok = ran(True)
        self.assertTrue(ok); self.assertTrue(stamp().endswith(f'(rewrite {"b" * 12})'))   # verdict holds for an iPhone too
        self.assertIsNone(check(False)); self.assertEqual(len(boots), 3)

    def test_main_tells_the_region_check_the_builds_target(self):   # the stamp and the verdict above depend on it
        text = (Path(REPO) / 'scripts/build_ipa_from_nsp.py').read_text()
        self.assertIn('not region_checked(g, patched, mac)', text); self.assertIn('g = region_check(repo, game, patched, work, env, logs, g, mac)', text)

    def test_region_check_sets_stdalloc_cap(self):   # set when the regions need it, worked out again by a later check, never over a value set by hand
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x118000000\n')
        self.derive(); patched = self.tmp / 'work/patched_v65536'; key = patched.with_name(patched.name + '.key')
        key.parent.mkdir(parents=True); key.write_text('a' * 64 + '\n')
        logs = self.tmp / 'logs'; logs.mkdir()
        def boot(*args): (logs / 'region_check.log').write_text('>>> nnMain\n'); (logs / 'regions.tsv').write_text('0x4000000\t0xa00000000\tmain+0x10\t0\t-\t-\t0\n'); return 0
        def check(need):
            with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), \
                 mock.patch.object(nsp.pg, 'stdalloc_cap_needed', return_value=need) if not isinstance(need, Exception) else mock.patch.object(nsp.pg, 'stdalloc_cap_needed', side_effect=need):
                return nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
        g = check(0xa4000000)
        self.assertEqual(g.get('stdalloc_cap'), '0xa4000000'); self.assertTrue(nsp.checked_cap(g))
        self.derive(); self.assertEqual(game.Game(self.pdir).get('stdalloc_cap'), '0xa4000000')   # a reused derivation keeps it
        g = check(0xa2000000); self.assertEqual(g.get('stdalloc_cap'), '0xa2000000')
        self.assertEqual(sum(l.startswith('stdalloc_cap\t') for l in g.table('game.tsv').read_text().splitlines()), 1)
        g = check(None); self.assertFalse(g.values('stdalloc_cap'))   # no longer needed: the value it set goes
        g = nsp.set_keys(g, {'stdalloc_cap': ('0xa0000000', 'by hand')}, 'Memory')
        g = check(AssertionError('a value set by hand is checked, not worked out again'))
        self.assertEqual(g.get('stdalloc_cap'), '0xa0000000')

    def test_the_heap_checks_stdalloc_cap_survives_the_region_check(self):   # issue #2830: the heap check limits StandardAllocator's region, not the pool the game reads as its memory
        with (self.src / 'game.tsv').open('a') as f: f.write('mem_alias\t0x118000000\nmem_reserve\t0x140000000\nmem_stack\t0x10000000\napp_pool\t0xCD500000\nsys_resource\t0x3000000\n')
        self.derive(); patched = self.tmp / 'work/patched_v65536'; key = patched.with_name(patched.name + '.key')
        key.parent.mkdir(parents=True); key.write_text('a' * 64 + '\n')
        g = self.check_boot('heap', '>>> nnMain\n[dm] [mem] alias region mapped up to +4.26 GB (alias region 4.38 GB)\n')
        self.assertEqual((g.get('stdalloc_cap'), g.get('app_pool')), ('0x40000000', '0xCD500000'))
        self.assertFalse(nsp.checked_cap(g))   # the region check checks it as a value set by hand rather than working it out again (and taking it out)
        logs = self.tmp / 'logs'
        def boot(*args): (logs / 'region_check.log').write_text('>>> nnMain\n'); (logs / 'regions.tsv').write_text('0x4000000\t0x40000000\tmain+0x10\t0\t-\t-\t0\n'); return 0
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), \
             mock.patch.object(nsp.pg, 'stdalloc_cap_needed', side_effect=AssertionError('a value the heap check set is checked, not worked out again')):
            g = nsp.region_check(REPO, self.ex, patched, self.tmp / 'work', {}, logs, game.Game(self.pdir))
        self.assertEqual(g.get('stdalloc_cap'), '0x40000000')

    def test_provisional_run_invalidates_derivation(self):
        self.derive()
        key = nsp.derived_key(self.pdir)
        self.assertTrue(key.is_file())
        nsp.set_keys(game.Game(self.pdir), {'mem_alias': ('0x80000000', '')}, 'Heap check')
        keys = self.tmp / 'prod.keys'; keys.write_text('test')
        fam = {'base': self.tmp / 'base.nsp', 'updates': [], 'dlc': []}
        args = nsp.parse_args([str(self.tmp), '-k', str(keys), '--platform', 'mac', '--provisional', '--profile-out', str(self.pdir.parent)])
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'apple_silicon', return_value=True), \
             mock.patch.object(nsp, '_importable', return_value=True), mock.patch.object(nsp.shutil, 'which', return_value='/dotnet'), \
             mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), mock.patch.object(nsp, 'decompress_nsz', return_value=fam), \
             mock.patch.object(nsp, 'nsp_versions', side_effect=RuntimeError('profile selected')):
            with self.assertRaisesRegex(RuntimeError, 'profile selected'): nsp.main(args)
        self.assertFalse(key.exists())
        self.derive()
        self.assertFalse(game.Game(self.pdir).values('mem_alias'))


class ProvisionalModules(Base):
    """provisional_modules: the subsdks the NROs import from go into modules, and the build row of this ExeFS's main names every module of modules the ExeFS has, which the host loads
    (game.py build_modules). Mario Golf: Super Rush's movie module went into modules alone and stayed unloaded (PR #2215)"""
    SUB = 'ee' * 16

    def setUp(self):
        super().setUp()
        self.ex = self.tmp / 'extract/v0/ExeFS'; nso(self.ex / 'main', BID); nso(self.ex / 'sdk', BID2); nso(self.ex / 'subsdk0', self.SUB)
        self.add = {}
        cm = mock.patch.object(nsp.pg, 'nro_modules', lambda exefs, nros, modules: {m: k for m, k in self.add.items() if m not in modules})
        cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def profile(self, modules, *rows):
        (self.src / 'game.tsv').write_text('\n'.join([f'title_id\t{TID:016X}', 'name\tTest Game', f'modules\t{modules}', *rows]) + '\n')
        return game.Game(self.src)

    def test_a_module_the_nros_import_from_goes_into_modules_and_the_row(self):
        self.add = {'subsdk0': 'NintendoSDK_movie'}
        g = nsp.provisional_modules(self.profile('main sdk', f'build\tv0\t4.0.0\tmain={BID}\tsdk={BID2}'), self.ex, {'a.nro': 'x'})
        self.assertEqual(g.words('modules'), ['main', 'subsdk0', 'sdk'])
        self.assertEqual(g.build_modules()[BID], ['main', 'subsdk0', 'sdk'])
        self.assertEqual(g.builds()['v0']['subsdk0'], self.SUB)

    def test_a_profile_written_before_gets_its_row_completed(self):   # modules already lists subsdk0, the row does not: nothing is added, the row is repaired
        g = nsp.provisional_modules(self.profile('main subsdk0 sdk', f'build\tv0\t4.0.0\tmain={BID}\tsdk={BID2}'), self.ex, {'a.nro': 'x'})
        self.assertEqual(g.build_modules()[BID], ['main', 'subsdk0', 'sdk'])
        g = nsp.provisional_modules(self.profile('main subsdk0 sdk', f'build\tv0\t4.0.0\tmain={BID}\tsdk={BID2}'), self.ex, {})   # also with no NROs
        self.assertEqual(g.build_modules()[BID], ['main', 'subsdk0', 'sdk'])

    def test_another_versions_row_is_left_as_it_is(self):   # a provisional profile is reused for the versions packed from its folder
        other = f'build\tv65536\t4.1.0\tmain={"77" * 16}\tsdk={BID2}'
        self.add = {'subsdk0': 'NintendoSDK_movie'}
        g = nsp.provisional_modules(self.profile('main sdk', other, f'build\tv0\t4.0.0\tmain={BID}\tsdk={BID2}'), self.ex, {'a.nro': 'x'})
        self.assertNotIn('subsdk0', g.builds()['v65536'])
        self.assertEqual(g.builds()['v0']['subsdk0'], self.SUB)

    def test_a_complete_profile_is_not_written(self):   # subsdk0 is in the ExeFS but not in modules: it stays out, and the file is not touched
        self.profile('main sdk', f'build\tv0\t4.0.0\tmain={BID}\tsdk={BID2}'); p = self.src / 'game.tsv'
        os.utime(p, ns=(10**18, 10**18))
        g = nsp.provisional_modules(game.Game(self.src), self.ex, {'a.nro': 'x'})
        self.assertEqual(p.stat().st_mtime_ns, 10**18)
        self.assertEqual(g.build_modules()[BID], ['main', 'sdk'])


class DlcOnlyShaders(Base):
    def test_only_dlc_rows_and_facts_can_change(self):
        tables = self.tmp / 'new-tables'; tables.mkdir()
        base = 'pkg.bin\t8\tcompute\tbase\t/base.shader\n'
        dlc = 'pkg.bin\t0\tvertex\tdlc\t/aoc/5/new.shader\n'
        old_facts = 'base\t{"local":[8,1,1]}\ndlc\t{"other":1}\n'
        self.src.joinpath('shader_map.tsv').write_text(base + dlc)
        self.src.joinpath('shader_facts.tsv').write_text(old_facts)
        g = game.Game(self.src)
        cases = [
            ('removed DLC and reindexed base', base.replace('\t8\t', '\t0\t'), 'base\t{"local":[8,1,1]}\n', True),
            ('added DLC', base + dlc + 'pkg.bin\t9\tpixel\tnew-dlc\t/aoc/7/new.shader\n', old_facts, True),
            ('shaderless DLC', base + dlc, old_facts, False),
            ('changed base key', base.replace('\tbase\t', '\tother\t'), old_facts, False),
            ('changed base source', base.replace('/base.shader', '/different.shader'), old_facts, False),
            ('changed base facts', base, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('no DLC difference', base + dlc, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('invalid map', 'not a shader map\n', old_facts, False),
        ]
        for name, shader_map, facts, want in cases:
            with self.subTest(name=name):
                tables.joinpath('shader_map.tsv').write_text(shader_map)
                tables.joinpath('shader_facts.tsv').write_text(facts)
                self.assertEqual(nsp.optional_shader_change(g, tables), want)

    def test_a_shader_the_dlc_shares_with_the_game(self):   # issue #2439: the split gives a shared shader the game's row (merge_keys), so both directions are DLC-only changes
        tables = self.tmp / 'new-tables'; tables.mkdir()
        game_rows = 'vs_sdf.bin\t0\tvertex\tbase\t/re_chunk_000.pak#1#SDF#var0\nvs_sdf.bin\t1\tvertex\tshared\t/re_chunk_000.pak#1#SDF#var619\n'
        dlc_rows = 'vs_other.bin\t0\tvertex\tdlc\t/aoc/1/re_dlc_000.pak#2#SDF#var1\n'
        facts = 'base\t{}\n'
        for name, profile, split, want in (
            ('registered with the DLC, packed without it', game_rows + dlc_rows, game_rows, True),
            ('registered without the DLC, packed with it', game_rows, game_rows + dlc_rows, True),
            ('registered under the old crediting (the shared shader as the DLC\'s), packed without it',   # such a profile is split again, not accepted
             game_rows.splitlines(True)[0] + dlc_rows + 'vs_other.bin\t1\tvertex\tshared\t/aoc/1/re_dlc_000.pak#2#SDF#var619\n', game_rows, False)):
            with self.subTest(name=name):
                self.src.joinpath('shader_map.tsv').write_text(profile); self.src.joinpath('shader_facts.tsv').write_text(facts)
                tables.joinpath('shader_map.tsv').write_text(split); tables.joinpath('shader_facts.tsv').write_text(facts)
                self.assertEqual(nsp.optional_shader_change(game.Game(self.src), tables), want)

    def test_play_pair_can_change_with_the_same_shader_map(self):
        tables = self.tmp / 'new-tables'; tables.mkdir()
        base = 'vs_other.bin\t0\tvertex\tbase\t/base.shader\nps_other.bin\t0\tpixel\tknown\t/known.shader\n'
        captured = 'vs_other.bin\t1\tvertex\tplayed\t/glslc_play/capture.glslc#GLSLC@0x0\n'
        for d in (self.src, tables):
            (d / 'shader_map.tsv').write_text(base + captured)
            (d / 'shader_facts.tsv').write_text('')
        (self.src / 'shader_pairs.tsv').write_text('')
        (tables / 'shader_pairs.tsv').write_text('played\tknown\n')
        g = game.Game(self.src)
        self.assertTrue(nsp.optional_shader_change(g, tables))
        (tables / 'shader_pairs.tsv').write_text('base\tknown\n')
        self.assertFalse(nsp.optional_shader_change(g, tables))
        (tables / 'shader_map.tsv').write_text(base)
        self.assertTrue(nsp.optional_shader_change(g, tables))

    def test_partial_build_includes_new_gameplay_captures(self):
        known, captured = 'a' * 16, 'b' * 16
        (self.src / 'shader_map.tsv').write_text(f'vs_other.bin\t0\tvertex\t{known}\t/base.shader\n'
                                                f'ps_other.bin\t0\tpixel\t{captured}\t/glslc_play/capture.glslc#GLSLC@0x0\n')
        (self.src / 'shader_seen.tsv').write_text(f'{known}\n')
        self.assertEqual(nsp.seen_keys(game.Game(self.src), self.tmp / 'shader', []), [known, captured])


class ComputeReaderAdditions(Base):
    V, P, C = 'a' * 16, 'b' * 16, 'c' * 16
    SOURCE = '/payload#Skinning#run.0'

    def setUp(self):
        super().setUp()
        self.tables = self.tmp / 'new-tables'; self.tables.mkdir()
        self.old = {'shader_map.tsv': f'vs_data.bin\t0\tvertex\t{self.V}\t/original-vs\nps_data.bin\t0\tpixel\t{self.P}\t/original-ps\n',
                    'shader_facts.tsv': f'{self.V}\t{{"manual":1}}\n', 'shader_pairs.tsv': f'{self.V}\t{self.P}\n',
                    'pso_pairs.tsv': '# no supplemental pairs\n'}
        self.new = dict(self.old)
        self.new['shader_map.tsv'] += f'cs_data.bin\t0\tcompute\t{self.C}\t{self.SOURCE}\n'
        self.new['shader_facts.tsv'] += f'{self.C}\t{{"local":[8,1,1],"shared":0}}\n'
        self.log = 'unity compute source: ' + json.dumps([self.C, self.SOURCE]) + '\n'
        for n, text in self.old.items(): (self.src / n).write_text(text)

    def check(self, tables=None, log=None):
        for n, text in (self.new if tables is None else tables).items(): (self.tables / n).write_text(text)
        return nsp.compute_reader_additions(game.Game(self.src), self.tables, self.log if log is None else log)

    def test_only_new_reader_proven_compute_kernels_qualify(self):
        self.assertTrue(self.check())
        changed = dict(self.new, **{'shader_map.tsv': self.new['shader_map.tsv'].replace('\t0\tvertex', '\t9\tvertex')})
        self.assertTrue(self.check(changed))  # Existing package indices may move; their identities cannot.
        changed['pso_pairs.tsv'] = f'{self.V}\t{self.P}\n'
        self.assertTrue(self.check(changed))  # Supplemental pass covers may be added beside the compute reader.
        for what, tables, log in (
            ('no additions', self.old, self.log),
            ('no reader evidence', self.new, ''),
            ('different source evidence', self.new, self.log.replace(self.SOURCE, '/other')),
            ('malformed evidence', self.new, 'unity compute source: broken\n'),
            ('wrong evidence type', self.new, 'unity compute source: {"key":"source"}\n'),
            ('graphics addition', dict(self.new, **{'shader_map.tsv': self.new['shader_map.tsv'].replace('\tcompute\t', '\tvertex\t')}), self.log),
            ('missing new facts', dict(self.new, **{'shader_facts.tsv': self.old['shader_facts.tsv']}), self.log),
            ('extra facts', dict(self.new, **{'shader_facts.tsv': self.new['shader_facts.tsv'] + 'other\t{}\n'}), self.log),
        ):
            with self.subTest(what=what): self.assertFalse(self.check(tables, log))

    def test_original_shaders_facts_and_pairs_cannot_change(self):
        for what, name, text in (
            ('removed shader', 'shader_map.tsv', self.new['shader_map.tsv'].split('\n', 1)[1]),
            ('changed key', 'shader_map.tsv', self.new['shader_map.tsv'].replace(self.V, 'd' * 16)),
            ('changed source', 'shader_map.tsv', self.new['shader_map.tsv'].replace('/original-vs', '/other')),
            ('changed package', 'shader_map.tsv', self.new['shader_map.tsv'].replace('vs_data.bin', 'vs_other.bin')),
            ('changed stage', 'shader_map.tsv', self.new['shader_map.tsv'].replace('\tvertex\t', '\tpixel\t')),
            ('duplicate key', 'shader_map.tsv', self.new['shader_map.tsv'] + self.old['shader_map.tsv']),
            ('changed fact', 'shader_facts.tsv', self.new['shader_facts.tsv'].replace('"manual":1', '"manual":2')),
            ('removed fact', 'shader_facts.tsv', self.new['shader_facts.tsv'].split('\n', 1)[1]),
            ('removed evidence pair', 'shader_pairs.tsv', '# none\n'),
            ('changed evidence pair', 'shader_pairs.tsv', f'{self.P}\t{self.V}\n'),
            ('compute in supplemental pair', 'pso_pairs.tsv', f'{self.V}\t{self.C}\n'),
        ):
            with self.subTest(what=what): self.assertFalse(self.check(dict(self.new, **{name: text})))
        (self.src / 'pso_pairs.tsv').write_text(f'{self.V}\t{self.P}\n')
        self.assertFalse(self.check())
        self.assertTrue(self.check(dict(self.new, **{'pso_pairs.tsv': f'{self.V}\t{self.P}\n'})))

    def test_registered_geometry_pairs_cannot_disappear(self):   # geo_pairs.tsv: a lost pair would lose its mesh library
        G = 'e' * 16
        (self.src / 'geo_pairs.tsv').write_text(f'{self.V}\t{G}\n')
        self.assertFalse(self.check(dict(self.new, **{'geo_pairs.tsv': '# none\n'})))
        self.assertTrue(self.check(dict(self.new, **{'geo_pairs.tsv': f'{self.V}\t{G}\n'})))


class SplitShaders(Base):
    NAMES = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')

    def setUp(self):
        super().setUp()
        self.repo = self.tmp / 'repo'
        for f in ('dm/gfx/tools/shader_split.py', 'dm/gfx/tools/dmsh_chain.lock', 'dm/games/game.py', 'dm/ios/tools/ios_metal.py'):
            p = self.repo / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('v1')
        self.pkg = self.tmp / 'work/shader/pkg'; self.logs = self.tmp / 'logs'; self.logs.mkdir()
        self.splits, self.rows, self.log = 0, 'derived', 'shaders: 3 (3 vertex)\n'
        cm = mock.patch.object(nsp, 'run', self.split); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def split(self, cmd, log, env=None, cwd=None, on_line=None):   # shader_split.py: the package and the four tables, deterministic for the same rows
        self.splits += 1; self.last_cmd = cmd; out = Path(cmd[5]); out.mkdir(parents=True); (out / 'main.bin').write_bytes(self.rows.encode())
        for n, t in zip(self.NAMES, cmd[6:]): Path(t).write_text(self.table(n))
        Path(cmd[8]).with_name('geo_pairs.tsv').write_text(self.table('geo_pairs.tsv'))   # written beside shader_pairs.tsv
        Path(log).write_text(self.log)

    def table(self, n):   # tests that set their own tables need not name geo_pairs.tsv
        if hasattr(self, 'tables'): return self.tables[n] if n != 'geo_pairs.tsv' else self.tables.get(n, '# none\n')
        return f'# split header\n{n}\t{self.rows}\n'

    def go(self, g, own=True, oodle=None, dlc_roms=(), derive_dlc=None, derive_compute=None, env=None, glslc_play=()):
        return nsp.split_shaders(sys.executable, self.repo, [self.ex / 'RomFS'], [self.ex / 'ExeFS/main'], self.pkg, env or {}, self.logs, g, own, oodle, dlc_roms, derive_dlc, derive_compute, glslc_play)

    def compute_addition(self):
        self.tables = {'shader_map.tsv': 'vs_data.bin\t0\tvertex\tbase\t/original\ncs_data.bin\t0\tcompute\tadded\t/assets#Compute#run.0\n',
                       'shader_facts.tsv': 'added\t{"local":[8,1,1],"shared":0}\n', 'shader_pairs.tsv': '# no pairs\n', 'pso_pairs.tsv': '# none\n'}
        old = dict(self.tables, **{'shader_map.tsv': self.tables['shader_map.tsv'].split('\n', 1)[0] + '\n', 'shader_facts.tsv': '# none\n'})
        for n, text in old.items(): (self.src / n).write_text(text)
        self.log = 'unity compute source: ' + json.dumps(['added', '/assets#Compute#run.0']) + '\nshaders: 2 (1 vertex, 1 compute)\n'
        return old

    def test_compute_reader_derivation_preserves_profile_and_reuses_only_verified_inputs(self):
        old = self.compute_addition(); g = game.Game(self.src)
        patch = self.src / 'patches.py'; patch.write_text(f'CUTS = {{"{BID}": []}}\n')
        def derive():
            nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
            return game.Game(self.pdir)
        promote, dlc = mock.Mock(side_effect=derive), mock.Mock()
        first = self.go(g, own=False, derive_dlc=dlc, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (1, 1)); dlc.assert_not_called()
        self.assertTrue(self.pkg.with_name('pkg.compute.key').is_file())
        self.assertFalse(self.pkg.with_name('pkg.dlc.key').exists())
        derived = game.Game(self.pdir)
        self.assertEqual(derived.rows, g.rows)
        self.assertEqual((self.pdir / 'patches.py').resolve(), patch.resolve())
        for n in self.NAMES:
            self.assertEqual(g.table(n).read_text(), old[n])
            self.assertEqual(derived.table(n).read_text(), self.tables[n])
        self.assertEqual(self.go(g, own=False, derive_compute=promote), first)
        self.assertEqual((self.splits, promote.call_count), (1, 2))
        (self.repo / 'dm/gfx/tools/shader_split.py').write_text('reader_version = 2')
        self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (2, 3))
        verifier = self.tmp / 'comparison-v2.py'; verifier.write_text('changed comparison code')
        with mock.patch.object(nsp, '__file__', str(verifier)):
            self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (3, 4))
        g.table('shader_map.tsv').write_text(old['shader_map.tsv'].replace('/original', '/changed'))
        for _ in range(2):
            with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
                self.go(g, own=False, derive_compute=promote)
        self.assertEqual((self.splits, promote.call_count), (5, 4))  # Stale proof cannot hide changes or failed checks.

    def test_compute_reader_derivation_rejects_missing_evidence_and_does_not_promote_own_profiles(self):
        self.compute_addition(); g = game.Game(self.src); promote = mock.Mock()
        self.log = 'shaders: 2 (1 vertex, 1 compute)\n'
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
            self.go(g, own=False, derive_compute=promote)
        promote.assert_not_called()
        own = self.own()
        self.go(own, derive_compute=promote)
        promote.assert_not_called()

    def test_dlc_only_derivation_is_incremental_and_keeps_registered_tables(self):
        base = 'other.bin\t1\tvertex\tbase\t/base.shader\n'
        old = {'shader_map.tsv': 'other.bin\t0\tvertex\tdlc\t/aoc/7/new.shader\n' + base,
               'shader_facts.tsv': 'base\t{"fact":1}\ndlc\t{"fact":2}\n',
               'shader_pairs.tsv': 'base\tdlc\n', 'pso_pairs.tsv': '# none\n'}
        for n in self.NAMES: (self.src / n).write_text(old[n])
        g = game.Game(self.src)
        self.tables = {'shader_map.tsv': base.replace('\t1\t', '\t0\t'), 'shader_facts.tsv': 'base\t{"fact":1}\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        def derive():
            nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
            return game.Game(self.pdir)
        promote = mock.Mock(side_effect=derive)
        first = self.go(g, own=False, derive_dlc=promote)
        self.assertEqual((self.splits, promote.call_count), (1, 1))
        self.assertEqual(self.go(g, own=False, derive_dlc=promote), first)
        self.assertEqual((self.splits, promote.call_count), (1, 2))   # proof of the strict non-DLC comparison is reused
        self.assertEqual(game.Game(self.pdir).rows, g.rows)
        for n in self.NAMES:
            self.assertEqual(g.table(n).read_text(), old[n])
            self.assertEqual(game.Game(self.pdir).table(n).read_text(), self.tables[n])
        # A new DLC extraction, even of the same file sizes, must perform the strict comparison again.
        dlc = self.tmp / 'aoc/7'; dlc.mkdir(parents=True); asset = dlc / 'shader'; asset.write_bytes(b'bytes')
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 2)
        self.redump(asset)
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 3)
        moved = dlc.with_name('8'); dlc.rename(moved)
        self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 4)   # index changes count even if every file statistic is identical
        verifier = self.tmp / 'comparison-v2.py'; verifier.write_text('changed comparison code')
        with mock.patch.object(nsp, '__file__', str(verifier)):
            self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 5)

    def test_base_shader_mismatch_never_derives_for_dlc(self):
        (self.src / 'shader_map.tsv').write_text('p.bin\t0\tvertex\tbase\t/base\np.bin\t1\tvertex\tdlc\t/aoc/7/shader\n')
        (self.src / 'shader_facts.tsv').write_text('# none\n')
        (self.src / 'shader_pairs.tsv').write_text('# none\n')
        self.tables = {'shader_map.tsv': 'p.bin\t0\tvertex\twrong-base\t/base\n', 'shader_facts.tsv': '# none\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        promote = mock.Mock()
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
            self.go(game.Game(self.src), own=False, derive_dlc=promote)
        promote.assert_not_called()

    def test_dlc_shaders_are_passed_and_invalidate_the_split(self):
        g = self.own(); self.go(g); self.assertEqual(self.splits, 1)
        dlc = self.tmp / 'work/aoc/7'; dlc.mkdir(parents=True)
        asset = dlc / 'shader.archive'; asset.write_bytes(b'new shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        self.assertEqual(json.loads(self.last_cmd[-1]), [str(dlc)])
        # Extra content has no main: the base RomFS/main pairing remains unchanged.
        self.assertEqual([json.loads(arg) for arg in self.last_cmd[3:5]], [[str(self.ex / 'RomFS')], [str(self.ex / 'ExeFS/main')]])
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        asset.write_bytes(b'updated DLC shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)   # removing the DLC removes its shaders from the union

    def test_gameplay_capture_content_invalidates_the_split(self):
        g = self.own(); self.go(g)
        folder = self.tmp / 'separate gameplay captures'; folder.mkdir()
        capture = folder / ('a' * 16 + '.glslc'); capture.write_bytes(b'first output')
        self.go(g, glslc_play=[folder]); self.assertEqual(self.splits, 2)
        self.assertEqual(json.loads(self.last_cmd[-2]), [])   # no DLC paths before the game-independent capture manifest
        self.go(g, glslc_play=[folder]); self.assertEqual(self.splits, 2)
        capture.write_bytes(b'other output')
        self.go(g, glslc_play=[folder]); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)

    def own(self):   # a derived profile with the empty tables, as main makes it
        self.derive(); g = game.Game(self.pdir); nsp.provisional_prep(sys.executable, self.repo, {}, self.logs, g); return g

    def test_own_profile_reuses_and_returns_the_package_key(self):
        g = self.own()
        k1 = self.go(g)
        self.assertEqual((self.splits, k1), (1, nsp.package_key(self.pkg)))
        for n in self.NAMES: self.assertEqual(g.table(n).read_text(), self.table(n))
        k2 = self.go(g)
        self.assertEqual((self.splits, k2), (1, k1))   # the key stored after the tables were written: the second run sees the same inputs

    def test_split_shaders_computes_package_key_once_per_hit_rebuild_or_replacement(self):
        g = self.own()
        calls = 0
        real_pkg_key = nsp.package_key
        def counting_pkg_key(pkg):
            nonlocal calls
            calls += 1
            return real_pkg_key(pkg)
        with mock.patch.object(nsp, 'package_key', side_effect=counting_pkg_key):
            # Fresh split: package_key computed once on the installed output.
            k1 = self.go(g)
            self.assertEqual(calls, 1)
            # Valid reuse hit: package_key computed once.
            k2 = self.go(g)
            self.assertEqual((calls, k2), (2, k1))
            # Replaced/tampered package triggers rebuild: 1 check on stale pkg + 1 on freshly installed output.
            (self.pkg / 'a.bin').write_bytes(b'tampered')
            k3 = self.go(g)
            self.assertEqual((calls, k3), (4, k1))
            # Disabled key (e.g. key=None): no package_key computation and returns None.
            with mock.patch.object(nsp, 'fingerprint', return_value=None):
                self.assertIsNone(self.go(g))
            self.assertEqual(calls, 4)

    def test_translation_and_profile_metadata_changes_keep_extraction(self):
        g = self.own(); before = self.go(g)
        for name in ('dmsh_build.py', 'dmsh_chain.py', 'pso_offline.py', 'unrelated_reader.py'):
            (self.repo / 'dm/gfx/tools' / name).write_text('version = 2\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        for name in ('README.md', 'bf_want.tsv', 'shader_facts.tsv.notes', 'dmsh_chain.h'):
            g.table(name).write_text('changed\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        g = nsp.set_keys(g, {'mem_used': ('0x4800000', ''), 'nvn_all_pulling': ('1', '')}, 'Runtime settings')
        self.assertEqual(self.go(g), before)
        self.assertEqual(self.splits, 1)

    def test_transitive_reader_dependency_changes_reextract(self):
        (self.repo / 'dm/gfx/tools/shader_split.py').write_text('import reader\n')
        (self.repo / 'dm/gfx/tools/reader.py').write_text('from reader_bits import FORMAT\n')
        helper = self.repo / 'dm/tools/reader_bits.py'; helper.parent.mkdir(parents=True)
        helper.write_text('FORMAT = 1\n')
        g = self.own(); self.go(g)
        helper.write_text('FORMAT = 2\n')
        self.go(g); self.assertEqual(self.splits, 2)
        helper.unlink()
        self.go(g); self.assertEqual(self.splits, 3)

    def test_extraction_settings_and_auxiliary_modules_reextract(self):
        g = self.own(); self.go(g)
        for i, (key, value) in enumerate((('shader_groups', 'foo=bar'), ('shader_note', 'Other sources'), ('shader_key_skip', '0x770+0x78'), ('modules', 'main sdk')), 2):
            g = nsp.set_keys(g, {key: (value, '')}, 'Extraction setting')
            self.go(g); self.assertEqual(self.splits, i)
        nso(self.ex / 'ExeFS/sdk', BID)
        self.go(g); self.assertEqual(self.splits, 6)
        nso(self.ex / 'ExeFS/sdk', BID2)
        self.go(g); self.assertEqual(self.splits, 7)

    def test_capture_overrides_are_inputs(self):
        g = self.own()
        for setting, suffix in (('DM_GLSLC_CAPTURE', '.glslc'), ('DM_UE_SHADERS', '.ushaderbytecode')):
            with self.subTest(setting=setting):
                capture = self.tmp / setting; capture.mkdir()
                file = capture / ('a' + suffix); file.write_bytes(b'first shader')
                env = {setting: str(capture)}
                self.go(g, env=env); before = self.splits
                file.write_bytes(b'new shader')
                self.go(g, env=env); self.assertEqual(self.splits, before + 1)
                self.go(g, env=env); self.assertEqual(self.splits, before + 1)
                moved = self.tmp / f'{setting}-moved'; shutil.copytree(capture, moved)   # the same files elsewhere: its contents are the input, not its location
                self.go(g, env={setting: str(moved)}); self.assertEqual(self.splits, before + 1)

    def test_missing_or_modified_package_is_not_reused(self):
        g = self.own(); self.go(g)
        output = self.pkg / 'main.bin'
        output.unlink()
        self.go(g); self.assertEqual(self.splits, 2)
        self.assertEqual(output.read_bytes(), b'derived')
        output.write_bytes(b'corrupt')
        self.go(g); self.assertEqual(self.splits, 3)
        self.assertEqual(output.read_bytes(), b'derived')

    def test_verified_decoder_rebuild_reuses_but_binary_edits_do_not(self):
        g = self.own()
        host = self.tmp / 'decoder-host'; host.write_bytes(b'host with old chain')
        modules = self.tmp / 'patched'; modules.mkdir()
        module = modules / 'main.patched.bin'; module.write_bytes(b'code')
        def provenance():
            host.with_name(host.name + '.decoder.json').write_text(json.dumps({'format': 1, 'binary': nsp.fingerprint(('files', [host])), 'inputs': 'a' * 64}))
        decoder = lambda: {'DM_OODLE_HOST': str(host), 'DM_OODLE_MODULES': str(modules)}
        provenance(); self.go(g, oodle=decoder)
        host.write_bytes(b'host with new chain'); provenance()
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 1)
        host.write_bytes(b'externally replaced')   # the old provenance no longer describes these bytes
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 2)
        before = module.stat()
        module.write_bytes(b'edit'); os.utime(module, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.go(g, oodle=decoder); self.assertEqual(self.splits, 3)

    def test_redumped_inputs_invalidate_only_own_split(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        captures = [self.ex / 'glslc_capture/a.glslc', self.ex / 'ue_shaders/a.ushaderbytecode']
        for p in captures: p.parent.mkdir(); p.write_bytes(b'shader')
        for own in (False, True):
            g = self.own() if own else game.Game(self.src)
            self.go(g, own=own)
            for p in [self.ex / 'RomFS/a.bin', *captures]:
                with self.subTest(own=own, path=p):
                    before = self.splits; self.redump(p); self.go(g, own=own)
                    self.assertEqual(self.splits, before + int(own))
                    self.go(g, own=own); self.assertEqual(self.splits, before + int(own))

    def test_decoder_host_and_modules_invalidate_split(self):
        g = self.own()
        host = self.tmp / 'decoder-host'; host.write_bytes(b'host')
        modules = self.tmp / 'patched'; modules.mkdir()
        module = modules / 'main.patched.bin'; module.write_bytes(b'code')
        decoder = mock.Mock(return_value={'DM_OODLE_HOST': str(host), 'DM_OODLE_MODULES': str(modules)})
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 1)
        host.write_bytes(b'fixed host')
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 2)
        module.write_bytes(b'new rewritten module')
        self.go(g, oodle=decoder); self.go(g, oodle=decoder); self.assertEqual(self.splits, 3)
        self.assertEqual(decoder.call_count, 6)

    def test_own_profile_changes_split_again(self):
        changes = {'manual table': lambda g: g.table('shader_facts_manual.tsv').write_text('k\t{"manual": 3}\n'),
                   'main': lambda g: nso(self.ex / 'ExeFS/main', 'ef' * 16),
                   'RomFS': lambda g: (self.ex / 'RomFS/b.bin').write_bytes(b'more'),
                   'tool': lambda g: (self.repo / 'dm/gfx/tools/shader_split.py').write_text('v2')}
        g = self.own(); self.go(g)
        for i, (what, change) in enumerate(changes.items(), 2):
            with self.subTest(what):
                change(g); self.assertEqual(self.go(g), nsp.package_key(self.pkg)); self.assertEqual(self.splits, i)
                self.go(g); self.assertEqual(self.splits, i)

    def test_own_generated_tables_are_checked(self):   # an edited or missing table of the split is never taken as is: the split's output comes back (split again, or restored)
        g = self.own(); self.go(g)
        for n in self.NAMES:
            with self.subTest(edited=n):
                g.table(n).write_text('# edited\nstale\trow\n'); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
            with self.subTest(missing=n):
                g.table(n).unlink(); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
        runs = self.splits; self.go(g); self.assertEqual(self.splits, runs)

    def test_own_new_output_replaces_tables(self):   # a changed input whose split writes other rows: the profile takes them
        g = self.own(); self.go(g)
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more'); self.go(g)
        self.assertEqual(g.table('shader_map.tsv').read_text(), self.table('shader_map.tsv'))
        self.go(g); self.assertEqual(self.splits, 2)

    def test_unreadable_files_are_reported(self):   # the split leaves out a file no reader reads and goes on: a provisional build says so and packs, a registered profile stops
        self.log = '[RED] 2 file(s) could not be read, their shaders are left out (first: /a.dict: not in the layout)\nshaders: 3 (3 vertex)\n'
        g, said = self.own(), []
        with mock.patch.object(nsp, 'say', said.append): self.go(g)
        self.assertEqual(len([m for m in said if m.startswith('[RED] 2 file(s) could not be read')]), 1)
        self.go(g); self.assertEqual(self.splits, 1)
        self.assertIn('2 file(s) unreadable', nsp.PROG.end.call_args[0][0])   # the reused split still names them
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        with self.assertRaisesRegex(nsp.BuildError, r'^2 file\(s\) could not be read'): self.go(game.Game(self.src), own=False)

    def test_allow_damaged_registered_split_still_requires_matching_tables(self):
        self.log = '[RED] 1 file(s) could not be read (first: archive: bad compressed texture)\nshaders: 3 (3 vertex)\n'
        for n in self.NAMES: (self.src / n).write_text(self.table(n))
        g = game.Game(self.src)
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True):
            self.go(g, own=False)
            self.go(g, own=False)
            self.assertEqual(self.splits, 1)
            self.rows = 'missing shader'; (self.ex / 'RomFS/b.bin').write_bytes(b'changed')
            with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
                self.go(g, own=False)
        with self.assertRaisesRegex(nsp.BuildError, r'^1 file\(s\) could not be read'):
            self.go(g, own=False)

    def test_mode_is_in_the_key(self):   # the same files split as an own profile, then checked against them as a registered one
        g = self.own(); self.go(g)
        self.assertEqual(self.go(g, own=False), nsp.package_key(self.pkg)); self.assertEqual(self.splits, 2)
        self.go(g, own=False); self.assertEqual(self.splits, 2)
        self.go(g); self.assertEqual(self.splits, 3)

    def test_registered_tables_are_compared(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(f'# older header\n{n}\t{self.rows}\n')
        g = game.Game(self.src)
        k = self.go(g, own=False)
        self.assertEqual((self.splits, k), (1, nsp.package_key(self.pkg)))
        self.assertEqual(self.go(g, own=False), k); self.assertEqual(self.splits, 1)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')   # a registered profile's tables are not written
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)   # a failed check is not reused
        self.assertEqual(self.splits, 3)

    def test_damaged_reuses_only_the_same_extraction(self):
        # --allow-damaged: a repaired copy keeps every path and size, which is all 'tree' sees, so a damaged RomFS is keyed by its files' times as well ('stat': an extraction writes
        # them again) and by the damage check's list. Before, a damaged build never reused, and its None key kept the shader step from reusing too: Ocarina of Time's three damaged
        # textures cost every build the split (56 s) and all 42,848 shaders restored from the cache (about ten minutes)
        g = self.own()
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True):
            k = self.go(g); self.assertEqual(k, nsp.package_key(self.pkg))   # the shader step's key: its libraries are reused while the package is the same
            self.assertEqual(self.go(g), k); self.assertEqual(self.splits, 1)
            self.redump(self.ex / 'RomFS/a.bin')   # a repaired copy: the same size, extracted again
            self.go(g); self.assertEqual(self.splits, 2)
            self.go(g); self.assertEqual(self.splits, 2)
            (self.ex / 'RomFS.img.formats').write_text('1 2\nTex/a.bntx.zs\t10\t00000000\n')   # check_formats' list beside the image
            self.go(g); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)   # an undamaged run takes nothing a damaged one keyed, and the other way round
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True): self.go(g)
        self.assertEqual(self.splits, 5)

    def test_missing_split_pairs_are_generated_again(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        g = game.Game(self.src)
        self.go(g, own=False)
        pairs = self.pkg.with_name('pso_pairs.tsv'); pairs.unlink()
        self.go(g, own=False)
        self.assertEqual(self.splits, 2)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))
        pairs.write_text('corrupt\tpair\n')
        self.go(g, own=False)
        self.assertEqual(self.splits, 3)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))
        geo = self.pkg.with_name('geo_pairs.tsv'); self.assertEqual(geo.read_text(), self.table('geo_pairs.tsv'))   # the vertex / geometry pairs likewise (mesh libraries)
        geo.unlink()
        self.go(g, own=False)
        self.assertEqual(self.splits, 4)
        self.assertEqual(geo.read_text(), self.table('geo_pairs.tsv'))

    def test_derived_rerun_is_incremental(self):   # the whole derived path of a second run: derive, prepare, rewrite keys, split
        g = self.own(); g = nsp.set_keys(g, {'mem_used': ('0x4800000', '')}, 'After the rewrite'); k = self.go(g)
        g = self.own(); self.assertEqual(g.get('mem_used'), '0x4800000')
        self.assertEqual(self.go(g), k); self.assertEqual(self.splits, 1)


class UeShaderDecodeStamp(Base):
    def setUp(self):
        super().setUp()
        sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
        sys.path.insert(0, str(REPO / 'dm/tools/tests'))
        import uepak
        import test_uepak_iostore as io_fix
        import test_uepak_v8 as v8_fix
        self.uepak = uepak
        self.io_fix = io_fix
        self.v8_fix = v8_fix
        self.work = self.tmp / 'work'; self.work.mkdir()
        self.logs = self.tmp / 'logs'; self.logs.mkdir()
        self.patched = self.tmp / 'patched'; self.patched.mkdir()
        self.g = game.Game(self.src)

    def _decode(self, d):
        class FakeOodle:
            def __init__(self, *a, **kw): pass
            def __call__(self, data, size): return data
            def close(self): pass
        with mock.patch.object(nsp, 'build_mac_host', return_value=self.tmp / 'dmhost'), \
             mock.patch.object(self.uepak, 'OodleHost', FakeOodle), \
             mock.patch.object(self.uepak, 'nso_image', return_value=(b'', 0, 0)), \
             mock.patch.object(nsp, 'say'):
            nsp.decode_ue_shaders(REPO, {'v0': d}, 'v0', self.patched, self.work, {}, self.logs, self.g)

    def _outputs(self, d):
        out = d / 'ue_shaders'
        return {p.name: p.read_bytes() for p in sorted(out.iterdir()) if p.name != 'stamp'}

    def _make_iostore(self, d, raw, *, part_size=0, packed=False):
        rdir = d / 'RomFS'
        if rdir.exists(): shutil.rmtree(rdir)
        (d / 'RomFS.img').unlink(missing_ok=True)
        rdir.mkdir(parents=True)
        self.io_fix.container(rdir, 'Game.utoc', [(self.io_fix.LIB, self.io_fix.library([(self.io_fix.GROUP, raw, raw)])),
                                                  (self.io_fix.GROUP, raw)], part_size=part_size)
        if packed:
            import romfs_image
            romfs_image.pack(rdir, d / 'RomFS.img')
            shutil.rmtree(rdir)

    def _make_pak(self, d, shader_bytes):
        rdir = d / 'RomFS'
        if rdir.exists(): shutil.rmtree(rdir)
        (d / 'RomFS.img').unlink(missing_ok=True)
        rdir.mkdir(parents=True)
        arch = self.v8_fix.v1_archive([(b'\x01' * 20, shader_bytes, len(shader_bytes), 3)])
        self.v8_fix.pak(rdir / 'Game.pak', [('Game/Content/ShaderArchive-Game-GLSL_SWITCH.ushaderbytecode', arch, False)])

    def test_equal_length_ucas_and_pak_replacements_match_clean_decode(self):
        # 1. Loose equal-length UCAS replacement
        d_inc = self.tmp / 'inc_ucas'; (d_inc / 'ExeFS').mkdir(parents=True); (d_inc / 'ExeFS/main').write_bytes(b'main-v1')
        d_clean = self.tmp / 'clean_ucas'; (d_clean / 'ExeFS').mkdir(parents=True); (d_clean / 'ExeFS/main').write_bytes(b'main-v1')
        raw1, raw2 = b'LSLG-shader-AAAA', b'LSLG-shader-BBBB'
        self.assertEqual(len(raw1), len(raw2))
        self._make_iostore(d_inc, raw1)
        self._decode(d_inc)
        # Unchanged rerun reuses without calling uepak.job
        with mock.patch.object(self.uepak, 'job', side_effect=AssertionError('should reuse unchanged UE shaders')):
            self._decode(d_inc)
        # Replace UCAS bytes with equal length and same mtime (ctime still changes via rewrite/replace)
        ucas = d_inc / 'RomFS/Game.ucas'
        old_data = ucas.read_bytes()
        st = ucas.stat()
        new_data = old_data.replace(raw1, raw2)
        self.assertEqual(len(new_data), len(old_data))
        ucas.write_bytes(new_data)
        os.utime(ucas, ns=(st.st_atime_ns, st.st_mtime_ns))
        self._decode(d_inc)
        self._make_iostore(d_clean, raw2)
        self._decode(d_clean)
        self.assertEqual(self._outputs(d_inc), self._outputs(d_clean))

        # 2. Loose equal-length pak replacement
        d_pak_inc = self.tmp / 'inc_pak'; (d_pak_inc / 'ExeFS').mkdir(parents=True); (d_pak_inc / 'ExeFS/main').write_bytes(b'main-v1')
        d_pak_clean = self.tmp / 'clean_pak'; (d_pak_clean / 'ExeFS').mkdir(parents=True); (d_pak_clean / 'ExeFS/main').write_bytes(b'main-v1')
        p1, p2 = b'LSLG-pak-1111111', b'LSLG-pak-2222222'
        self.assertEqual(len(p1), len(p2))
        self._make_pak(d_pak_inc, p1)
        self._decode(d_pak_inc)
        self._make_pak(d_pak_inc, p2)
        self._decode(d_pak_inc)
        self._make_pak(d_pak_clean, p2)
        self._decode(d_pak_clean)
        self.assertEqual(self._outputs(d_pak_inc), self._outputs(d_pak_clean))

    def test_numbered_partition_packed_image_warm_check_and_legacy_stamp(self):
        # Numbered _s1.ucas partition replacement invalidates stamp
        d = self.tmp / 'split_ucas'; (d / 'ExeFS').mkdir(parents=True); (d / 'ExeFS/main').write_bytes(b'main-v1')
        raw1 = b'LSLG-part-payload-' + b'A' * 64
        raw2 = b'LSLG-part-payload-' + b'B' * 64
        self._make_iostore(d, raw1, part_size=128)
        part1 = d / 'RomFS/Game_s1.ucas'
        self.assertTrue(part1.is_file())
        self._decode(d)
        first_out = self._outputs(d)
        part1.write_bytes(part1.read_bytes().replace(b'A', b'B'))
        self._decode(d)
        self.assertNotEqual(self._outputs(d), first_out)
        self.assertIn(raw2, next(iter(self._outputs(d).values())))

        # Legacy ue-archives-v2 stamp is treated as a miss and migrated
        (d / 'ue_shaders/stamp').write_text('ue-archives-v2\nGame.utoc\t10\n')
        with mock.patch.object(self.uepak, 'job', wraps=self.uepak.job) as job_spy:
            self._decode(d)
        self.assertEqual(job_spy.call_count, 1)
        self.assertTrue((d / 'ue_shaders/stamp').read_text().startswith('ue-archives-v3\n'))

        # Packed RomFS.img warm check does not scan full image payload and still invalidates on replacement
        d_img = self.tmp / 'packed_ucas'; (d_img / 'ExeFS').mkdir(parents=True); (d_img / 'ExeFS/main').write_bytes(b'main-v1')
        self._make_iostore(d_img, b'LSLG-img-AAAA', packed=True)
        self._decode(d_img)
        img_out1 = self._outputs(d_img)
        real_open = open
        img_path = str((d_img / 'RomFS.img').resolve())
        reads = []
        def tracking_open(file, *a, **kw):
            f = real_open(file, *a, **kw)
            if isinstance(file, (str, os.PathLike)) and os.path.abspath(os.fspath(file)) == img_path:
                orig_read = f.read
                def counted_read(n=-1):
                    data = orig_read(n)
                    reads.append(len(data))
                    return data
                f.read = counted_read
            return f
        with mock.patch('builtins.open', side_effect=tracking_open), \
             mock.patch.object(self.uepak, 'job', side_effect=AssertionError('should reuse packed image decode')):
            self._decode(d_img)
        self.assertLessEqual(sum(reads), 16, 'warm check must not scan full RomFS.img payload')
        self._make_iostore(d_img, b'LSLG-img-BBBB', packed=True)
        self._decode(d_img)
        self.assertNotEqual(self._outputs(d_img), img_out1)

    def test_mid_decode_input_replacement_preserves_previous_good_output(self):
        d = self.tmp / 'toctou_ucas'; (d / 'ExeFS').mkdir(parents=True); (d / 'ExeFS/main').write_bytes(b'main-v1')
        raw1, raw2, raw3 = b'LSLG-good-1111', b'LSLG-mid--2222', b'LSLG-new--3333'
        self._make_iostore(d, raw1)
        self._decode(d)
        prev_out = self._outputs(d)
        prev_stamp = (d / 'ue_shaders/stamp').read_text()

        # Trigger a decode for raw2, then replace the source with raw3 after finish() but before swap_in()
        self._make_iostore(d, raw2)
        real_finish = self.uepak.finish
        def finish_then_replace(jd, out):
            res = real_finish(jd, out)
            self._make_iostore(d, raw3)
            return res
        with mock.patch.object(self.uepak, 'finish', side_effect=finish_then_replace):
            with self.assertRaisesRegex(nsp.BuildError, 'changed while decoding'):
                self._decode(d)
        self.assertEqual(self._outputs(d), prev_out)
        self.assertEqual((d / 'ue_shaders/stamp').read_text(), prev_stamp)
        self.assertFalse((d / 'ue_shaders.partial').exists())
        # Subsequent run decodes raw3 cleanly and publishes its matching stamp
        self._decode(d)
        self.assertIn(raw3, next(iter(self._outputs(d).values())))


class RewriteWorkers(Base):
    def test_explicit_workers_override_the_automatic_module_budget(self):
        g = game.Game(self.src)
        for value, expected in (('1', '1'), ('3', '3'), ('', '14')):
            with self.subTest(value=value):
                out = self.tmp / f'patched-{value or "auto"}'
                env = {'DM_GAME': str(self.src), 'DM_REWRITE_J': value}
                with mock.patch.object(nsp, 'collect_nros', return_value={}), mock.patch.object(nsp, 'patch_files', return_value=[]), \
                     mock.patch.object(nsp.os, 'cpu_count', return_value=14), mock.patch.object(nsp.time, 'sleep'), \
                     mock.patch.object(nsp.proc_group, 'Group', return_value=mock.Mock(p=mock.Mock(returncode=0, poll=lambda: 0), finish=lambda: 0, stragglers=[])) as popen:
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', out, env, self.tmp / 'logs', g)
                self.assertEqual(popen.call_args.kwargs['env']['DM_REWRITE_J'], expected)
                self.assertEqual(env['DM_REWRITE_J'], value)

    def test_invalid_workers_fail_before_collecting_modules(self):
        for value in ('0', '-1', 'many'):
            with self.subTest(value=value), mock.patch.object(nsp, 'collect_nros') as collect:
                with self.assertRaisesRegex(nsp.BuildError, 'DM_REWRITE_J must be a positive integer'):
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', self.tmp / 'patched',
                                {'DM_REWRITE_J': value}, self.tmp / 'logs', game.Game(self.src))
                collect.assert_not_called()


class CollectNrosDiscovery(Base):
    def test_nrr_discovery_matches_across_loose_tree_and_image_and_cleans_manifest(self):
        import romfs as rfs
        import romfs_image

        # 1. Empty tree -> no matches and removes any stale manifest/key
        empty = self.tmp / 'empty_romfs'; empty.mkdir()
        dst = self.tmp / 'nro_out'; dst.mkdir()
        (dst / 'nro.tsv').write_text('nro0\t00\t1\ta.nrr\tloose\n')
        kf = dst.with_name(dst.name + '.key'); kf.write_text('stale\n')
        self.assertEqual(nsp.collect_nros(Path(sys.executable), REPO, rfs.Path(empty), dst, {}, self.tmp / 'logs'), {})
        self.assertFalse(dst.exists())
        self.assertFalse(kf.exists())

        # 2. Loose tree with .nrr directory, mixed-case suffix, nested files, valid file symlink, and broken symlink
        loose = self.tmp / 'loose_romfs'; loose.mkdir()
        (loose / '.nrr').mkdir()
        (loose / '.nrr/zeta.nrr').write_bytes(b'NRR0-z')
        (loose / '.nrr/alpha.nrr').write_bytes(b'NRR0-a')
        (loose / '.nrr/UPPER.NRR').write_bytes(b'NRR0-u')
        (loose / 'sub/deep').mkdir(parents=True)
        (loose / 'sub/deep/ok.nrr').write_bytes(b'NRR0-d')
        (loose / 'sub/Mixed.Nrr').write_bytes(b'NRR0-m')
        (loose / 'sub/target.bin').write_bytes(b'NRR0-link')
        # Pack equivalent image before adding broken symlink
        img = self.tmp / 'RomFS.img'
        (loose / 'sub/link.nrr').symlink_to(loose / 'sub/target.bin')
        romfs_image.pack(loose, img)
        (loose / 'sub/broken.nrr').symlink_to(loose / 'sub/does_not_exist')

        loose_root = rfs.Path(loose)
        img_root = rfs.Path(img)
        loose_rel = [p.relative_to(loose_root).as_posix() for p in nsp._nrr_files(loose_root)]
        img_rel = [p.relative_to(img_root).as_posix() for p in nsp._nrr_files(img_root)]
        expected = ['.nrr/alpha.nrr', '.nrr/zeta.nrr', 'sub/deep/ok.nrr', 'sub/link.nrr']
        self.assertEqual(loose_rel, expected)
        self.assertEqual(img_rel, expected)
        self.assertEqual(loose_rel, sorted(p.relative_to(loose_root).as_posix() for p in loose_root.rglob('*.nrr') if p.is_file()))

        # 3. collect_nros runs when NRRs exist, reuses when unchanged, and cleans dst/key when the last match disappears
        runs = []
        def fake_run(cmd, log, env=None):
            runs.append(cmd)
            out = Path(cmd[3]); out.mkdir(parents=True, exist_ok=True)
            (out / 'nro.tsv').write_text('nro0\t' + '00' * 32 + '\t64\talpha.nrr\tloose\n')
            (out / 'nro0.nro').write_bytes(b'NRO0')
        with mock.patch.object(nsp, 'run', side_effect=fake_run):
            got = nsp.collect_nros(Path(sys.executable), REPO, loose_root, dst, {}, self.tmp / 'logs')
            self.assertEqual(set(got), {'nro0'})
            self.assertEqual(len(runs), 1)
            got2 = nsp.collect_nros(Path(sys.executable), REPO, loose_root, dst, {}, self.tmp / 'logs')
            self.assertEqual(got2, got)
            self.assertEqual(len(runs), 1)
        # Remove every matching .nrr file (leaving .nrr directory, UPPER.NRR, and broken.nrr)
        for rel in expected: (loose / rel).unlink()
        self.assertEqual(nsp.collect_nros(Path(sys.executable), REPO, loose_root, dst, {}, self.tmp / 'logs'), {})
        self.assertFalse(dst.exists())
        self.assertFalse(kf.exists())


class LayerShaders(Base):
    def test_redumped_romfs_and_captures_invalidate_only_own_libraries(self):
        cap = self.ex / 'capture'; cap.mkdir(); (cap / 'shader.bin').write_bytes(b'shader')
        self.derive(); g = game.Game(self.pdir)
        logs = self.tmp / 'logs'; logs.mkdir(); runs = []
        def translate(cmd, log, **kw):
            api = Path(cmd[1]).stem.split('_')[0]
            out = Path(cmd[-1]) / api
            out.mkdir(parents=True)
            (out / f'{api}_shaders.metallib').write_bytes(b'library')
            (out / f'{api}_shaders.tsv').write_text('# table\n')
            Path(log).write_text('macosx: 1\n'); runs.append(api)
        with mock.patch.object(nsp, 'run', side_effect=translate), mock.patch.object(nsp, 'toolchain', return_value=()), \
             mock.patch.object(nsp.shutil, 'which', return_value='/glslangValidator'), \
             mock.patch.object(nsp.subprocess, 'run', return_value=mock.Mock(stdout='compiler', stderr='')):
            for api, build in (('vk', nsp.build_vk_shaders), ('gl', nsp.build_gl_shaders)):
                roots = [self.ex / 'RomFS', cap] + ([self.ex / 'ExeFS/main'] if api == 'gl' else [])
                for own in (False, True):
                    sh = self.tmp / f'{api}-{own}'
                    go = partial(build, sys.executable, REPO, roots, sh, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=own)
                    go(); go()
                    for p in (self.ex / 'RomFS/a.bin', cap / 'shader.bin'):
                        with self.subTest(api=api, own=own, path=p):
                            before = len(runs); self.redump(p); go()
                            self.assertEqual(len(runs), before + int(own))
                            go(); self.assertEqual(len(runs), before + int(own))

    def test_gl_table_only_main_detects_missing_romfs_shaders(self):
        tsv = self.tmp / 'gl_shaders.tsv'
        self.assertFalse(nsp.gl_table_only_main(tsv))
        tsv.write_text('# GL shader ABI 2\n0123456789abcdef\tfrag\tr:-1:0\tmain#0\n')
        self.assertTrue(nsp.gl_table_only_main(tsv))
        tsv.write_text('# GL shader ABI 2\n0123456789abcdef\tfrag\tr:-1:0\tmain#0\nfedcba9876543210\tfrag\tr:-1:0\t/shaders/sprite.glsl\n')
        self.assertFalse(nsp.gl_table_only_main(tsv))

    def test_packed_gl_romfs_img_invalidates_and_reuses_with_controls(self):
        import romfs_image
        self.derive(); g = game.Game(self.pdir)
        logs = self.tmp / 'logs'; logs.mkdir(); runs = []
        def translate(cmd, log, **kw):
            out = Path(cmd[-1]) / 'gl'
            out.mkdir(parents=True, exist_ok=True)
            (out / 'gl_shaders.metallib').write_bytes(b'library')
            (out / 'gl_shaders.tsv').write_text('# GL shader ABI 3\n')
            Path(log).write_text('macosx: 1\n'); runs.append(cmd)
        src = self.tmp / 'gl_src'; (src / 'shaders').mkdir(parents=True)
        (src / 'shaders/a.frag').write_text('#version 330\nvoid main() {}\n')
        img = self.ex / 'RomFS.img'
        romfs_image.pack(src, img)
        loose = self.tmp / 'gl_loose'; (loose / 'shaders').mkdir(parents=True)
        (loose / 'shaders/a.frag').write_text('#version 330\nvoid main() {}\n')
        with mock.patch.object(nsp, 'run', side_effect=translate), mock.patch.object(nsp, 'toolchain', return_value=()), \
             mock.patch.object(nsp.shutil, 'which', return_value='/glslangValidator'), \
             mock.patch.object(nsp.subprocess, 'run', return_value=mock.Mock(stdout='compiler', stderr='')):
            for prov in (False, True):
                with self.subTest(provisional=prov):
                    sh = self.tmp / f'gl-img-{prov}'
                    roots = [nsp.rfs.Path(img), self.ex / 'ExeFS/main']
                    go = partial(nsp.build_gl_shaders, sys.executable, REPO, roots, sh, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=prov)
                    runs.clear()
                    go(); self.assertEqual(len(runs), 1)
                    go(); self.assertEqual(len(runs), 1)   # untouched image reuses
                    (src / 'shaders/b.frag').write_text('#version 330\nvoid main() {}\n')
                    romfs_image.pack(src, img)
                    go(); self.assertEqual(len(runs), 2)   # added GLSL file repacked to same path triggers rebuild
                    go(); self.assertEqual(len(runs), 2)
            # Controls: loose directory and ExeFS/main
            sh_loose = self.tmp / 'gl-loose'
            lroots = [loose, self.ex / 'ExeFS/main']
            runs.clear()
            nsp.build_gl_shaders(sys.executable, REPO, lroots, sh_loose, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=False)
            nsp.build_gl_shaders(sys.executable, REPO, lroots, sh_loose, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=False)
            self.assertEqual(len(runs), 1)
            (loose / 'shaders/b.frag').write_text('#version 330\nvoid main() {}\n')
            nsp.build_gl_shaders(sys.executable, REPO, lroots, sh_loose, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=False)
            self.assertEqual(len(runs), 2)
            nso(self.ex / 'ExeFS/main', BID)
            nsp.build_gl_shaders(sys.executable, REPO, lroots, sh_loose, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=False)
            self.assertEqual(len(runs), 3)

    def test_replacing_packed_dlc_equal_length_shader_rebuilds_registered_build(self):
        import romfs_image
        self.derive(); g = game.Game(self.pdir)
        logs = self.tmp / 'logs'; logs.mkdir(); runs = []
        def translate(cmd, log, **kw):
            api = Path(cmd[1]).stem.split('_')[0]
            out = Path(cmd[-1]) / api
            out.mkdir(parents=True, exist_ok=True)
            (out / f'{api}_shaders.metallib').write_bytes(b'library')
            (out / f'{api}_shaders.tsv').write_text('# table\n')
            Path(log).write_text('macosx: 1\n'); runs.append(cmd)
        base_src = self.tmp / 'base_romfs'; (base_src / 'shaders').mkdir(parents=True)
        (base_src / 'shaders/base.frag').write_text('#version 330\nvoid main(){int b=0;}\n')
        base_img = self.ex / 'RomFS.img'
        romfs_image.pack(base_src, base_img)
        with mock.patch.object(nsp, 'run', side_effect=translate), mock.patch.object(nsp, 'toolchain', return_value=()), \
             mock.patch.object(nsp.shutil, 'which', return_value='/glslangValidator'), \
             mock.patch.object(nsp.subprocess, 'run', return_value=mock.Mock(stdout='compiler', stderr='')):
            cases = (
                ('gl', nsp.build_gl_shaders, 'shaders/dlc.frag', b'#version 330\nvoid main(){int a=1;}\n', b'#version 330\nvoid main(){int b=2;}\n'),
                ('vk', nsp.build_vk_shaders, 'shaders/dlc.spv', b'\x03\x02\x23\x07AAAA', b'\x03\x02\x23\x07BBBB'),
            )
            for api, build, rel, first, second in cases:
                self.assertEqual(len(first), len(second))
                with self.subTest(api=api):
                    dlc_src = self.tmp / f'{api}_dlc_src'; (dlc_src / 'shaders').mkdir(parents=True)
                    (dlc_src / rel).write_bytes(first)
                    dlc_img = self.tmp / f'{api}_aoc/1.img'; dlc_img.parent.mkdir(parents=True)
                    romfs_image.pack(dlc_src, dlc_img)
                    sh = self.tmp / f'{api}-dlc-img'
                    roots = [nsp.rfs.Path(base_img), self.ex / 'ExeFS/main']
                    def go(dlc_list, build=build, roots=roots, sh=sh):
                        build(sys.executable, REPO, roots, sh, {'WORK': str(self.tmp), 'PATH': ''}, logs, g, mac=True, provisional=False, dlc_roms=dlc_list)
                    runs.clear()
                    go([dlc_img]); self.assertEqual(len(runs), 1)
                    self.assertIn(dlc_img, runs[-1])
                    go([dlc_img]); self.assertEqual(len(runs), 1)   # unchanged image reuses
                    (dlc_src / rel).write_bytes(second)
                    romfs_image.pack(dlc_src, dlc_img)
                    go([dlc_img]); self.assertEqual(len(runs), 2)   # equal-length replacement rebuilds once
                    go([dlc_img]); self.assertEqual(len(runs), 2)   # following unchanged run reuses
                    moved = dlc_img.with_name('2.img'); dlc_img.rename(moved)
                    go([moved]); self.assertEqual(len(runs), 3)     # DLC index change invalidates even with identical bytes
                    go([moved]); self.assertEqual(len(runs), 3)
                    # Loose DLC directory control: equal-length replacement with fresh mtime also rebuilds once
                    loose_dlc = self.tmp / f'{api}_aoc_loose/1'; (loose_dlc / 'shaders').mkdir(parents=True)
                    (loose_dlc / rel).write_bytes(first)
                    go([loose_dlc]); self.assertEqual(len(runs), 4)
                    go([loose_dlc]); self.assertEqual(len(runs), 4)
                    (loose_dlc / rel).write_bytes(second); self.redump(loose_dlc / rel)
                    go([loose_dlc]); self.assertEqual(len(runs), 5)
                    go([loose_dlc]); self.assertEqual(len(runs), 5)


class MacApp(Base):
    def test_packer_reports_reuse_and_first_pack_steps(self):
        t = self.tmp; out = t / 'Game.app'; packs = []
        def pack(cmd, log, env=None, on_line=None):
            if not on_line: return   # the launcher settings (dm/games/settings.py)
            packs.append(cmd)
            (out / 'Contents/MacOS').mkdir(parents=True, exist_ok=True)
            (out / 'Contents/MacOS/dmhost').write_bytes(b'host')
            if len(packs) == 1: on_line('step: launcher and host (clones of the game files)')
            on_line(f'app: {out}' + (', reused (inputs unchanged)' if len(packs) > 1 else ''))
        with mock.patch.object(nsp, 'run', side_effect=pack):
            for i in range(2):
                result = nsp.mac_app(sys.executable, REPO, out, TID, t, self.ex, t / 'patched', t / 'host', t / 'font', t / 'shader', None, 'vulkan', 'en-US',
                                     game.Game(self.src), 'Game', '1.0.0', 'org.nxnative.test', None, '', {}, t)
                self.assertIsNone(result)
                if i: nsp.PROG.end.assert_called_with('Game.app, reused (inputs unchanged)')
                else: nsp.PROG.set.assert_called_with(step='launcher and host (clones of the game files)')
        self.assertEqual(len(packs), 2)   # pack_app owns the reuse decision and sidecar key


class GlslcCapture(Base):
    def test_the_capture_boot_presses_a(self):   # Crash Team Racing Nitro-Fueled compiles its run-time programs only after its title screen has had a button press
        g = game.Game(self.src); ex = self.tmp / 'extract/v0'; (ex / 'RomFS').mkdir(parents=True); logs = self.tmp / 'logs'; logs.mkdir()
        out = bytearray(0x48); out[:4] = b'\x91\x68\x86\x19'; out[0x44:0x48] = (0x48).to_bytes(4, 'little')   # a whole GLSLC output: its magic and its own length
        seen = {}
        def boot(host, patched, henv, rd, log, secs, done):
            seen.update(henv); cap = Path(henv['DM_GLSLC_CAPTURE']); cap.mkdir(parents=True); (cap / '0123456789abcdef.glslc').write_bytes(out); return None
        with mock.patch.object(nsp, 'build_mac_host'), mock.patch.object(nsp, 'boot_mac', side_effect=boot), mock.patch.object(nsp, 'PROG'):
            nsp.capture_glslc(REPO, {'v0': ex}, 'v0', self.tmp / 'patched', self.tmp / 'work', {}, logs, g)
        self.assertEqual(seen['DM_KEYS'], nsp.GLSLC_KEYS)
        self.assertTrue(nsp.GLSLC_KEYS.startswith('5:A'))   # the first press comes well within the run (GLSLC_SECS)
        self.assertEqual(sorted(p.name for p in (ex / 'glslc_capture').iterdir()), ['0123456789abcdef.glslc'])


class GlslcCacheCoverage(Base):
    KEY = '0123456789abcdef'

    def capture(self, rc=None, observed=True, required=False, partial=False, capture_error=False):
        work, logs = self.tmp / 'work', self.tmp / 'logs'; logs.mkdir(exist_ok=True)
        def boot(host, patched, env, rd, log, secs, done):
            shmap = Path(env['DM_NVN_SHMAP'])
            if required:
                self.assertEqual(shmap, self.src / 'shader_map.tsv')
            else:
                self.assertEqual(shmap.read_text(), '# GLSLC capture: observe submitted shaders\n')
            if partial:
                cap = Path(env['DM_GLSLC_CAPTURE'])
                cap.mkdir()
                (cap / 'aaaaaaaaaaaaaaaa.glslc').write_bytes(b'partial output')
            text = f'[dm] [nvn-mtl] [RED] shader {self.KEY} is not in the shader map\n' if observed else ''
            if capture_error:
                text += '[dm] [RED] glslc: unknown output header\n'
            log.write_text(text)
            return rc
        with mock.patch.object(nsp, 'build_mac_host', return_value=work / 'dmhost'), mock.patch.object(nsp, 'boot_mac', side_effect=boot):
            return nsp.capture_glslc(REPO, {'v65536': self.ex}, 'v65536', work / 'patched', work, {}, logs, game.Game(self.src))

    def test_surviving_boot_defers_to_offline_coverage(self):
        observed = self.capture()
        self.assertEqual(observed, {self.KEY})
        (self.src / 'shader_map.tsv').write_text(f'pkg.bin\t0\tvertex\t{self.KEY}\t/cache#GLSLC@0x80\n')
        nsp.check_glslc_coverage(observed, game.Game(self.src))

    def test_empty_capture_still_fails_without_boot_evidence(self):
        for rc in (139, 0):
            with self.subTest(rc=rc), self.assertRaisesRegex(nsp.BuildError, 'captured only 0'):
                self.capture(rc=rc)
        for kwargs in ({'observed': False}, {'partial': True}, {'capture_error': True}):
            with self.subTest(**kwargs), self.assertRaisesRegex(nsp.BuildError, 'captured only 0'):
                self.capture(**kwargs)

    def test_registered_capture_requirements_are_not_relaxed(self):
        (self.src / 'shader_map.tsv').write_text(f'pkg.bin\t0\tvertex\t{self.KEY}\t/glslc_capture/aaaaaaaaaaaaaaaa.glslc#GLSLC@0x0\n')
        with self.assertRaisesRegex(nsp.BuildError, 'profile needs 1'):
            self.capture(required=True)

    def test_missing_offline_key_and_no_observations_fail(self):
        for observed, msg in (({self.KEY}, 'observed shaders absent'), (set(), 'no submitted shader keys')):
            with self.subTest(observed=observed), self.assertRaisesRegex(nsp.BuildError, msg):
                nsp.check_glslc_coverage(observed, game.Game(self.src))


class JitBootDone(unittest.TestCase):
    def test_needs_the_boot_code_and_ten_seconds_with_nothing_new(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp.time, 'time', return_value=100.0) as now:
            part = Path(t); done = nsp.jit_boot_done(part)
            self.assertFalse(done())   # nothing captured yet
            (part / 'small.jit').write_bytes(b'x'); now.return_value = 200.0
            self.assertFalse(done())   # a request, but not the boot code
            (part / 'boot.jit').write_bytes(bytes((1 << 20) + 1)); now.return_value = 201.0
            self.assertFalse(done())   # the boot code, just now
            now.return_value = 211.0
            self.assertFalse(done())
            now.return_value = 211.5
            self.assertTrue(done())
            (part / 'late.jit').write_bytes(b'x')
            self.assertFalse(done())   # something new starts the wait again


class ShaderProfileKey(Base):   # the shader step's key takes the profile's shader inputs (dmsh_chain.py --inputs), not every file's bytes
    def key(self):
        env = dict(os.environ, DM_GAME=f'{TID:016X}', DM_GAMES_DIR=str(self.tmp / 'games'))
        return nsp.fingerprint(nsp.shader_profile_key(sys.executable, REPO, env, game.Game(self.src)))

    def test_files_no_shader_build_reads_keep_the_key(self):   # a note edit used to restore every library
        before = self.key()
        (self.src / 'README.md').write_text('# a note\n')
        (self.src / 'pso_manifest.tsv').write_text('S\tk2\tpixel\n')
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'pad_layout\tfull\n')
        self.assertEqual(self.key(), before)

    def test_shader_inputs_change_the_key(self):
        changes = {
            'shader map': lambda: (self.src / 'shader_map.tsv').write_text('# registered\npkg.bin\t0\tvertex\tother\tx\n'),
            'geo_mesh.tsv': lambda: (self.src / 'geo_mesh.tsv').write_text('a\tb\n'),
            'an nvn_ key': lambda: (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'nvn_test\ton\n'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                before = self.key(); change(); self.assertNotEqual(self.key(), before)



class BuildSources(unittest.TestCase):   # profile_guess.py build_sources reads the head of every file of the RomFS: 19 s of every build of Ocarina of Time, kept beside the image
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.t)
        self.game = self.t / 'v0'; (self.game / 'ExeFS').mkdir(parents=True)
        (self.game / 'ExeFS/main').write_bytes(b'code'); (self.game / 'RomFS.img').write_bytes(b'image')
        self.reader = self.t / 'reader.py'; self.reader.write_text('# the readers\n')
        self.calls, self.src = [], {'graphics': ('nvn', 'main imports nvnBootstrapLoader'), 'glslc_capture': (False, ''), 'ue_decode': (0, 'no shader code archive'), 'modules': (['main'], '')}
        def read(exefs, romfs, mods): self.calls.append((exefs, romfs, list(mods))); return dict(self.src)
        for patch in (mock.patch.object(nsp, 'pg', mock.Mock(build_sources=read)), mock.patch.object(nsp, 'python_tool_files', lambda *a: [self.reader])):
            patch.start(); self.addCleanup(patch.stop)

    def sources(self, mods=('main',)): return nsp.build_sources(REPO, self.game, list(mods))

    def test_kept_while_the_image_the_modules_and_the_readers_are_the_same(self):
        self.assertEqual(self.sources(), self.src)
        self.assertEqual(self.calls, [(self.game / 'ExeFS', nsp.rfs.source(self.game), ['main'])])
        got = self.sources(); self.assertEqual(len(self.calls), 1)   # from RomFS.img.sources
        self.assertEqual(got, self.src); self.assertEqual(got['graphics'][0], 'nvn'); self.assertIs(got['glslc_capture'][0], False)
        changes = (lambda: (self.game / 'RomFS.img').write_bytes(b'another image'), lambda: (self.game / 'ExeFS/main').write_bytes(b'c0de'),
                   lambda: (self.game / 'ExeFS/subsdk0').write_bytes(b'more'), lambda: self.reader.write_text('# other readers\n'),
                   lambda: (self.game / 'RomFS.img.sources').write_text('not what it wrote'))
        for n, change in enumerate(changes, 2):
            with self.subTest(change=n):
                change(); self.assertEqual(self.sources(), self.src); self.assertEqual(len(self.calls), n)
                self.sources(); self.assertEqual(len(self.calls), n)
        self.sources(('main', 'sdk')); self.assertEqual(len(self.calls), 7)   # the modules the profile loads
        st = (self.game / 'RomFS.img').stat(); os.utime(self.game / 'RomFS.img', ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))   # the image written again, the same size
        self.sources(('main', 'sdk')); self.assertEqual(len(self.calls), 8)

    def test_a_romfs_folder_is_read_each_time(self):
        (self.game / 'RomFS.img').unlink(); (self.game / 'RomFS').mkdir()
        self.sources(); self.sources(); self.assertEqual(len(self.calls), 2)
        self.assertEqual(sorted(p.name for p in self.game.iterdir()), ['ExeFS', 'RomFS'])

    def test_what_json_does_not_give_back_is_not_kept(self):
        self.src['nro_imports'] = (frozenset({'glslcCompile'}), '')
        self.assertEqual(self.sources(), self.src); self.assertEqual(self.sources(), self.src); self.assertEqual(len(self.calls), 2)
        self.assertFalse((self.game / 'RomFS.img.sources').exists())


class DlcDecoderSources(unittest.TestCase):
    """DLC decoder-archive discovery works for both .img and directory DLC, caches outside aoc/, and wires DM_OODLE_HOST into split_shaders."""

    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.t)
        self.work = self.t / 'work'; self.work.mkdir()
        self.game = self.work / 'extract/v0'; (self.game / 'ExeFS').mkdir(parents=True)
        (self.game / 'ExeFS/main').write_bytes(b'code')
        self.aoc = self.work / 'aoc'; self.aoc.mkdir()
        self.reader = self.t / 'reader.py'; self.reader.write_text('# readers\n')

    @staticmethod
    def _forge(ver):
        return b'scimitar\0' + ver.to_bytes(4, 'little') + bytes(16)

    def _pack_dlc(self, name, files):
        import romfs_image
        src = self.t / f'src_{name}'; src.mkdir(parents=True, exist_ok=True)
        for rel, data in files.items():
            p = src / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
        img = self.aoc / name
        romfs_image.pack(src, img)
        return img

    def test_image_dlc_cached_outside_aoc_and_invalidated(self):
        img = self._pack_dlc('1.img', {'ACB/DataNX.forge': self._forge(25)})
        before = sorted(p.name for p in self.aoc.iterdir())
        calls = []
        real = profile_guess.decoder_sources
        def spy(roots, mains=()): calls.append((list(roots), list(mains))); return real(roots, mains=mains)
        with mock.patch.object(nsp.pg, 'decoder_sources', side_effect=spy), \
             mock.patch.object(nsp, 'python_tool_files', lambda *a: [self.reader]):
            got = nsp.dlc_decoder_sources(REPO, self.work, self.game, [img])
            self.assertTrue(got['oodle_packs'][0], got)
            self.assertEqual(len(calls), 1)
            self.assertEqual(sorted(p.name for p in self.aoc.iterdir()), before)   # memo must not sit inside aoc/
            self.assertTrue((self.work / 'aoc.sources').is_file())
            self.assertEqual(nsp.dlc_decoder_sources(REPO, self.work, self.game, [img]), got)
            self.assertEqual(len(calls), 1)   # cache hit
            changes = (
                lambda: self._pack_dlc('1.img', {'ACB/DataNX.forge': self._forge(25), 'extra.bin': b'1'}),
                lambda: os.utime(img, ns=(img.stat().st_atime_ns, img.stat().st_mtime_ns + 1_000_000_000)),
                lambda: (self.game / 'ExeFS/main').write_bytes(b'c0de'),
                lambda: self.reader.write_text('# readers v2\n'),
                lambda: (self.work / 'aoc.sources').write_text('corrupt'),
            )
            for n, change in enumerate(changes, 2):
                with self.subTest(change=n):
                    change()
                    nsp.dlc_decoder_sources(REPO, self.work, self.game, [img])
                    self.assertEqual(len(calls), n)
            img2 = self._pack_dlc('2.img', {'ACR/placeholder.txt': b'x'})
            nsp.dlc_decoder_sources(REPO, self.work, self.game, [img, img2])
            self.assertEqual(len(calls), 7)

    def test_directory_dlc_read_each_time_without_writing_cache(self):
        dlc_dir = self.aoc / '1'; (dlc_dir / 'ACB').mkdir(parents=True)
        (dlc_dir / 'ACB/DataNX.forge').write_bytes(self._forge(25))
        calls = []
        real = profile_guess.decoder_sources
        def spy(roots, mains=()): calls.append(list(roots)); return real(roots, mains=mains)
        with mock.patch.object(nsp.pg, 'decoder_sources', side_effect=spy):
            self.assertTrue(nsp.dlc_decoder_sources(REPO, self.work, self.game, [dlc_dir])['oodle_packs'][0])
            self.assertTrue(nsp.dlc_decoder_sources(REPO, self.work, self.game, [dlc_dir])['oodle_packs'][0])
        self.assertEqual(len(calls), 2)
        self.assertFalse((self.work / 'aoc.sources').exists())

    def test_main_enables_oodle_host_from_image_or_directory_dlc(self):
        class Stop(Exception): pass
        dump = self.t / 'dump'; (dump / 'base/exefs').mkdir(parents=True); (dump / 'base/romfs').mkdir(parents=True)
        (dump / 'base/exefs/main').write_bytes(b'NSO0' + bytes(0x80))
        npdm = bytearray(0xa0); npdm[0:4] = b'META'; npdm[0xc] = 1
        npdm[0x70:0x78] = (0x80).to_bytes(4, 'little') + (0x20).to_bytes(4, 'little')
        npdm[0x80:0x84] = b'ACI0'; npdm[0x90:0x98] = TID.to_bytes(8, 'little')
        (dump / 'base/exefs/main.npdm').write_bytes(bytes(npdm))
        (dump / 'base/romfs/placeholder').write_bytes(b'x')
        prof_dir = self.t / 'profiles' / f'{TID:016X}'; prof_dir.mkdir(parents=True)
        (prof_dir / 'game.tsv').write_text(f'title_id\t{TID:016X}\nname\tTest Game\nipa\tTest.ipa\nmodules\tmain\nbuild\tv0\t1.0\tmain={BID}\nshader_versions\tv0\n')
        g = game.Game(prof_dir)

        def run_to_split(dlc_setup, base_oodle=False):
            work = self.t / f'run_{len(os.listdir(self.t))}'
            aoc = work / 'aoc'; aoc.mkdir(parents=True)
            dlc_setup(aoc)
            said, captured = [], {}
            def fake_extract(ex, ver, src, app):
                d = ex / ver; (d / 'ExeFS').mkdir(parents=True, exist_ok=True); (d / 'RomFS').mkdir(exist_ok=True)
                nso(d / 'ExeFS/main', BID); return d
            def fake_split(*a, **k):
                captured['oodle'] = a[9]
                captured['oodle_env'] = a[9]() if a[9] else None
                captured['dlc_roms'] = a[10]
                raise Stop()
            src_stub = {'graphics': ('nvn', 'NVN'), 'glslc_capture': (False, ''), 'vk_capture': (False, ''),
                        'gl_capture': (False, ''), 'gl_fixed': (False, ''), 'ue_decode': (False, ''),
                        'oodle_packs': (base_oodle, 'base oodle' if base_oodle else ''), 'ue_legacy': (False, '')}
            args = nsp.parse_args([str(dump), '--platform', 'mac', '--work-dir', str(work), '--title-id', f'{TID:016X}'])
            with mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.t / 'profiles')}), \
                 mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), \
                 mock.patch.object(nsp, 'PROD_KEYS', ''), mock.patch.object(nsp, 'apple_silicon', return_value=True), \
                 mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'check_metal'), \
                 mock.patch.object(nsp, 'extract_dump', side_effect=fake_extract), mock.patch.object(nsp, 'check_formats'), \
                 mock.patch.object(nsp, 'check_versions_aarch64'), mock.patch.object(nsp, 'build_mismatch', return_value=[]), \
                 mock.patch.object(nsp, 'build_sources', return_value=src_stub), mock.patch.object(nsp, 'extract_aoc', return_value=aoc), \
                 mock.patch.object(nsp, 'rewrite', return_value=g), mock.patch.object(nsp, 'chain_header'), \
                 mock.patch.object(nsp, 'build_mac_host', return_value=work / 'tools/dmhost') as build_host, \
                 mock.patch.object(nsp, 'split_shaders', side_effect=fake_split), \
                 mock.patch.object(nsp, 'say', side_effect=said.append), \
                 mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked')):
                with self.assertRaises(Stop): nsp.main(args)
            return captured, said, build_host

        import romfs_image
        def make_img(ver):
            def setup(aoc):
                src = aoc.parent / 'tmp_src'; (src / 'ACB').mkdir(parents=True)
                (src / 'ACB/DataNX.forge').write_bytes(self._forge(ver))
                romfs_image.pack(src, aoc / '1.img')
            return setup
        def make_dir(ver):
            def setup(aoc):
                (aoc / '1/ACB').mkdir(parents=True)
                (aoc / '1/ACB/DataNX.forge').write_bytes(self._forge(ver))
            return setup

        for label, setup in (('img-v25', make_img(25)), ('dir-v25', make_dir(25))):
            with self.subTest(case=label):
                cap, said, build_host = run_to_split(setup)
                self.assertIsNotNone(cap['oodle'])
                self.assertEqual(Path(cap['oodle_env']['DM_OODLE_HOST']).name, 'dmhost')
                self.assertTrue(cap['oodle_env']['DM_OODLE_MODULES'])
                build_host.assert_called_once()
                self.assertTrue(any(s.startswith('DLC: 1 Anvil forge(s)') for s in said), said)

        for label, setup in (('img-v27-lzo', make_img(27)), ('dir-v27-lzo', make_dir(27))):
            with self.subTest(case=label):
                cap, said, _ = run_to_split(setup)
                self.assertIsNone(cap['oodle'])
                self.assertFalse(any(s.startswith('DLC: ') for s in said), said)

        with mock.patch.object(nsp, 'dlc_decoder_sources', side_effect=AssertionError('should not scan DLC when base already triggers Oodle')):
            cap, _, _ = run_to_split(make_img(25), base_oodle=True)
            self.assertIsNotNone(cap['oodle'])


class StreamedFingerprints(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def _ref_files_fp(self, paths):
        h = hashlib.sha256()
        h.update(b'\0files\0')
        for p in map(nsp.rfs.Path, paths):
            h.update(p.name.encode() + b'\0')
            try: h.update(hashlib.sha256(p.read_bytes()).digest())
            except OSError: h.update(b'missing')
        return h.hexdigest()

    def test_streamed_fingerprint_matches_whole_file_and_missing_error_handling(self):
        import romfs_image
        empty = self.tmp / 'empty.bin'; empty.write_bytes(b'')
        loose = self.tmp / 'loose.bin'; loose.write_bytes(b'hello-' * 500000)
        missing = self.tmp / 'does_not_exist.bin'
        src = self.tmp / 'rsrc'; src.mkdir()
        (src / 'in_img.bin').write_bytes(b'romfs-data-' * 120000)
        (src / 'img_empty.bin').write_bytes(b'')
        img = self.tmp / 'RomFS.img'; romfs_image.pack(src, img)
        paths = [empty, missing, loose, nsp.rfs.Path(img / 'in_img.bin'), nsp.rfs.Path(img / 'img_empty.bin'), nsp.rfs.Path(img / 'missing.bin')]
        self.assertEqual(nsp.fingerprint(('files', paths)), self._ref_files_fp(paths))

        # An OSError mid-stream must still produce the exact missing-file digest without corrupting the outer hash.
        expected_missing = self._ref_files_fp([self.tmp / 'nonexistent' / loose.name])
        class FailingStream:
            def __enter__(self): return self
            def __exit__(self, *a): return False
            def read(self, n): raise OSError('disk read error')
        with mock.patch.object(nsp.rfs, 'open', return_value=FailingStream()):
            self.assertEqual(nsp.fingerprint(('files', [loose])), expected_missing)

    def test_streamed_fingerprint_has_bounded_peak_allocation(self):
        big = self.tmp / 'big.bin'
        big.write_bytes(b'x' * (6 << 20))
        tracemalloc.start()
        try:
            tracemalloc.clear_traces()
            digest = nsp.fingerprint(('files', [big]))
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(digest, self._ref_files_fp([big]))
        self.assertLess(peak, 3 << 20)


if __name__ == '__main__': unittest.main()
