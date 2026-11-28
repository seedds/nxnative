"""NSP script --work-dir / WORK_DIR: the build folder is the folder given, as it is, and the IPA (or Mac app) defaults to the folder holding it; without it the
build folder is <title ID>_build in the NSP folder and the IPA is in the NSP folder. The provisional profile goes in <build folder>/profile (copied once from the
default place when --work-dir is new). The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

TID = 0x010015100B514000   # a game with a profile in dm/games


def profile_ipa():
    profile = game.by_title(TID)
    assert profile, 'no profile for the test game'
    return profile.get('ipa')


class Stop(Exception): pass


class WorkDir(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.tmp = Path(td.name).resolve()
        self.nsps = self.tmp / 'nsps'; self.nsps.mkdir()
        (self.tmp / 'prod.keys').write_text('test')
        env = mock.patch.dict(os.environ, {}, clear=False); env.start(); self.addCleanup(env.stop)
        os.environ.pop('WORK', None)

    def plan(self, *opts, setting=''):
        """What main decides before it extracts anything: DM_ROOT (the build folder) and the IPA path the console reports; self.nsz_work is the build folder
        the NSZ conversions were given (None: the default); self.seen holds the env handed to check_prereqs"""
        fam = {'base': self.nsps / 'base.nsp', 'updates': [], 'dlc': []}
        args = nsp.parse_args([str(self.nsps), '-k', str(self.tmp / 'prod.keys'), *opts])
        said, seen = [], {}
        def stop(env, *a): seen.update(env); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), \
             mock.patch.object(nsp, 'decompress_nsz', return_value=fam) as nsz, mock.patch.object(nsp, 'nsp_versions', return_value={'v0': fam['base']}), \
             mock.patch.object(nsp, 'WORK_DIR', setting), mock.patch.object(nsp, 'check_prereqs', side_effect=stop), \
             mock.patch.object(nsp, 'say', side_effect=said.append):
            with self.assertRaises(Stop): nsp.main(args)
        self.nsz_work = nsz.call_args.args[5]
        self.seen = seen
        self.said = said
        ipa = next(m.split(': ', 1)[1] for m in said if m.startswith('output IPA: '))
        return Path(seen['DM_ROOT']), Path(ipa)

    def test_default(self):
        ipa = profile_ipa()
        self.assertEqual(self.plan(), (self.nsps / f'{TID:016X}_build', self.nsps / ipa))
        self.assertIsNone(self.nsz_work)

    def test_option_moves_the_build_folder_and_the_default_ipa(self):
        ipa = profile_ipa()
        out = self.tmp / 'builds' / 'mine'
        self.assertEqual(self.plan('--work-dir', str(out)), (out, out.parent / ipa))
        self.assertEqual(self.nsz_work, out)   # the NSZ conversions are build files too

    def test_setting_does_the_same(self):
        ipa = profile_ipa()
        out = self.tmp / 'elsewhere'
        self.assertEqual(self.plan(setting=str(out)), (out, out.parent / ipa))
        self.assertEqual(self.nsz_work, out)

    def test_out_names_the_ipa_whatever_the_build_folder(self):
        out, ipa = self.plan('--work-dir', str(self.tmp / 'w'), '-o', str(self.tmp / 'x.ipa'))
        self.assertEqual((out, ipa), (self.tmp / 'w', self.tmp / 'x.ipa'))

    def test_profile_folder(self):
        args = lambda *o: nsp.parse_args([str(self.nsps), *o])
        default = self.nsps / f'{TID:016X}_build' / 'profile'
        with mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROFILE_OUT', ''), mock.patch.object(nsp, 'say'):
            self.assertEqual(nsp.profile_folder(args(), self.nsps, TID), default)
            work = self.tmp / 'w'
            self.assertEqual(nsp.profile_folder(args('--work-dir', str(work)), self.nsps, TID), work / 'profile')
            self.assertFalse(work.exists())   # no profile to migrate: nothing is made
            elsewhere = self.tmp / 'p'
            self.assertEqual(nsp.profile_folder(args('--work-dir', str(work), '--profile-out', str(elsewhere)), self.nsps, TID), elsewhere)
            # the setting does the same as the option
            with mock.patch.object(nsp, 'WORK_DIR', str(work)):
                self.assertEqual(nsp.profile_folder(args(), self.nsps, TID), work / 'profile')

    def test_the_builds_own_profile_is_not_taken_as_registered(self):
        # a provisional or derived profile in the folder this script writes them to, reached through DM_GAMES_DIR (the bring-up recipe the script prints; DM_GAMES_FIRST as a
        # derived build sets it): main took it for a registered one, and derived it onto itself (Chained Echoes)
        own, ipa = self.nsps / f'{TID:016X}_build' / 'profile', profile_ipa(); (own / f'{TID:016X}').mkdir(parents=True)
        mine = game.by_title(TID); assert mine, 'no profile for the test game'
        text = (mine.dir / 'game.tsv').read_text(encoding='utf-8')
        (own / f'{TID:016X}' / 'game.tsv').write_text(text.replace(f'ipa\t{ipa}\n', 'ipa\tOwnCopy.ipa\n'), encoding='utf-8')
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(own), 'DM_GAMES_FIRST': '1'}), mock.patch.object(nsp, 'PROFILE_OUT', ''):
            found = game.by_title(TID); assert found
            self.assertEqual(found.get('ipa'), 'OwnCopy.ipa')   # what the lookup finds without the rule
            self.assertIn(own, nsp.own_profile_folders(nsp.parse_args([str(self.nsps)]), self.nsps, TID))
            self.assertIn(own, nsp.own_profile_folders(nsp.parse_args([str(self.nsps), '--work-dir', str(self.tmp / 'w')]), self.nsps, TID))   # a profile from before --work-dir too
            self.assertEqual(self.plan()[1], self.nsps / ipa)   # dm/games' profile, not the own copy

    def test_profile_migrates_once_and_keeps_the_original(self):
        old = self.nsps / f'{TID:016X}_build' / 'profile' / f'{TID:016X}'; old.mkdir(parents=True)
        (old / 'game.tsv').write_text('old')
        work = self.tmp / 'w'
        with mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROFILE_OUT', ''), mock.patch.object(nsp, 'say'):
            got = nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(work)]), self.nsps, TID)
            self.assertEqual(got, work / 'profile')
            self.assertEqual((got / f'{TID:016X}' / 'game.tsv').read_text(), 'old')
            self.assertEqual((old / 'game.tsv').read_text(), 'old')   # the original stays
            (got / f'{TID:016X}' / 'game.tsv').write_text('edited'); (old / 'game.tsv').write_text('newer old')
            nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(work)]), self.nsps, TID)
            self.assertEqual((got / f'{TID:016X}' / 'game.tsv').read_text(), 'edited')   # a profile there is not overwritten
            other = self.tmp / 'x'; (other / 'profile').mkdir(parents=True)
            nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(other)]), self.nsps, TID)
            self.assertEqual(list((other / 'profile').iterdir()), [])   # a folder that already has a profile is left alone


class ScratchWork(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.tmp = Path(td.name).resolve()
        self.nsps = self.tmp / 'nsps'; self.nsps.mkdir()
        (self.tmp / 'prod.keys').write_text('test')
        env = mock.patch.dict(os.environ, {}, clear=False); env.start(); self.addCleanup(env.stop)
        os.environ.pop('WORK', None)

    def run_main_to_prereqs(self, on_prereqs):
        fam = {'base': self.nsps / 'base.nsp', 'updates': [], 'dlc': []}
        args = nsp.parse_args([str(self.nsps), '-k', str(self.tmp / 'prod.keys')])
        said = []
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), \
             mock.patch.object(nsp, 'decompress_nsz', return_value=fam), mock.patch.object(nsp, 'nsp_versions', return_value={'v0': fam['base']}), \
             mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'check_prereqs', side_effect=on_prereqs), \
             mock.patch.object(nsp, 'say', side_effect=said.append):
            with self.assertRaises(Stop): nsp.main(args)
        return said

    def test_unset_and_blank_work_default_to_build_tmp(self):
        for raw in (None, '', '   '):
            with self.subTest(work=raw):
                if raw is None: os.environ.pop('WORK', None)
                else: os.environ['WORK'] = raw
                seen: dict[str, str] = {}
                def stop(env, *a, out=seen): out.update(env); raise Stop()
                said = self.run_main_to_prereqs(stop)
                self.assertEqual(Path(seen['WORK']), Path(seen['DM_ROOT']) / 'tmp')
                self.assertFalse(any(m.startswith('scratch folder (WORK): ') for m in said))

    def test_caller_work_uses_task_owned_subdir_and_cleans_up_on_exit(self):
        scratch = self.tmp / 'scratch'; scratch.mkdir()
        keep_file = scratch / 'keep.txt'; keep_file.write_text('caller data')
        keep_dir = scratch / 'other_tool'; keep_dir.mkdir(); (keep_dir / 'a').write_text('ok')
        os.environ['WORK'] = str(scratch)
        own_dir: Path | None = None
        exists_during = False
        pid_text = ''
        def stop(env, *a):
            nonlocal own_dir, exists_during, pid_text
            own_dir = Path(env['WORK'])
            exists_during = own_dir.is_dir()
            pid_text = (own_dir / 'pid').read_text().strip()
            (own_dir / 'dmsh_leftover').mkdir(); (own_dir / 'dmsh_leftover' / 'x.spv').write_bytes(b'x')
            raise Stop()
        said = self.run_main_to_prereqs(stop)
        assert own_dir is not None
        self.assertTrue(exists_during)
        self.assertEqual(pid_text, str(os.getpid()))
        self.assertEqual(own_dir.parent, scratch)
        self.assertTrue(own_dir.name.startswith(f'{nsp.SCRATCH_PREFIX}{TID:016X}-'))
        self.assertEqual(os.environ['WORK'], str(scratch))   # caller env is not mutated
        self.assertIn(f'scratch folder (WORK): {own_dir}', said)
        self.assertFalse(own_dir.exists())   # cleaned on unwind
        self.assertEqual(keep_file.read_text(), 'caller data')   # caller entries preserved
        self.assertEqual((keep_dir / 'a').read_text(), 'ok')

    def test_relative_work_is_resolved_and_created(self):
        cwd = os.getcwd(); os.chdir(self.tmp); self.addCleanup(os.chdir, cwd)
        os.environ['WORK'] = 'rel_scratch/nested'
        seen: list[Path] = []
        def stop(env, *a): seen.append(Path(env['WORK'])); raise Stop()
        self.run_main_to_prereqs(stop)
        expected_root = (self.tmp / 'rel_scratch/nested').resolve()
        self.assertTrue(expected_root.is_dir())
        self.assertEqual(seen[0].parent, expected_root)
        self.assertFalse(seen[0].exists())

    def test_file_as_work_raises_build_error(self):
        bad = self.tmp / 'not_a_dir'; bad.write_text('file')
        os.environ['WORK'] = str(bad)
        def fail(*a): raise Stop()
        with self.assertRaisesRegex(nsp.BuildError, 'WORK is not a folder'):
            self.run_main_to_prereqs(fail)

    def test_reentrant_main_isolates_and_cleans_each_invocation(self):
        scratch = self.tmp / 'shared'; scratch.mkdir()
        caller_file = scratch / 'keep'; caller_file.write_text('stay')
        os.environ['WORK'] = str(scratch)
        records: list[tuple[int, Path]] = []
        depth = 0
        def nested(env, *a):
            nonlocal depth
            own = Path(env['WORK'])
            marker = own / 'marker.txt'; marker.write_text(f'depth={depth}')
            records.append((depth, own))
            if depth == 0:
                depth = 1
                self.run_main_to_prereqs(nested)   # simulates other_programs() reentering main()
                # Inner cleaned itself up without touching outer's active task folder.
                self.assertFalse(records[1][1].exists())
                self.assertTrue(own.is_dir())
                self.assertEqual(marker.read_text(), 'depth=0')
            raise Stop()
        self.run_main_to_prereqs(nested)
        self.assertEqual(len(records), 2)
        self.assertNotEqual(records[0][1], records[1][1])
        self.assertFalse(records[0][1].exists())
        self.assertFalse(records[1][1].exists())
        self.assertEqual(caller_file.read_text(), 'stay')

    def test_sweep_only_removes_dead_pid_task_owned_folders(self):
        root = self.tmp / 'root'; root.mkdir()
        dead = root / f'{nsp.SCRATCH_PREFIX}dead-1'; dead.mkdir(); (dead / 'pid').write_text('999999\n'); (dead / 'f').write_text('x')
        live = root / f'{nsp.SCRATCH_PREFIX}live-1'; live.mkdir(); (live / 'pid').write_text(f'{os.getpid()}\n')
        no_pid = root / f'{nsp.SCRATCH_PREFIX}nopid-1'; no_pid.mkdir()
        bad_pid = root / f'{nsp.SCRATCH_PREFIX}badpid-1'; bad_pid.mkdir(); (bad_pid / 'pid').write_text('0\n')
        other = root / 'caller_dir'; other.mkdir(); (other / 'pid').write_text('999999\n')
        target = self.tmp / 'elsewhere'; target.mkdir(); (target / 'pid').write_text('999999\n')
        link = root / f'{nsp.SCRATCH_PREFIX}link-1'; link.symlink_to(target)
        real_kill = os.kill
        def fake_kill(pid, sig):
            if pid == 999999: raise ProcessLookupError()
            return real_kill(pid, sig)
        with mock.patch.object(os, 'kill', side_effect=fake_kill):
            nsp.sweep_scratch_work(root)
        self.assertFalse(dead.exists())
        self.assertTrue(live.is_dir())
        self.assertTrue(no_pid.is_dir())
        self.assertTrue(bad_pid.is_dir())
        self.assertTrue(other.is_dir())
        self.assertTrue(link.is_symlink())
        self.assertTrue(target.is_dir())


if __name__ == '__main__':
    unittest.main()
