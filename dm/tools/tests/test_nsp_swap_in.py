"""swap_in, the NSP script's publication of a finished output: whatever stops it, the destination holds the whole old output, the whole new one, or nothing, and a rerun
publishes. Each call runs in a process of its own (fixtures/swap_in_run.py) with a fault injected into its renames or unlinks, or a SIGKILL."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
RUN = Path(__file__).resolve().parent / 'fixtures/swap_in_run.py'
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'


def tree(p, tag):   # 3 folders of 4 files, each file naming its tree: 12 unlinks to delete it
    for d in range(3):
        (p / f'd{d}').mkdir(parents=True)
        for f in range(4): (p / f'd{d}' / f'f{f}').write_text(f'{tag} {d} {f}\n')


def state(p):   # 'old', 'new', 'absent', or 'mixed' (anything else: a partial tree)
    if not p.exists(): return 'absent'
    files = sorted(p.rglob('*'))
    got = {x.relative_to(p).as_posix(): x.read_text() for x in files if x.is_file()}
    for tag in ('old', 'new'):
        if got == {f'd{d}/f{f}': f'{tag} {d} {f}\n' for d in range(3) for f in range(4)}: return tag
    return 'mixed'


class SwapIn(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory(); self.root = Path(self.td.name)
        self.tmp, self.dst, self.old = self.root / 'new.partial', self.root / 'out', self.root / 'out.old.partial'

    def tearDown(self): self.td.cleanup()

    def swap(self, fault='none'):
        r = subprocess.run([sys.executable, RUN, SCRIPT, self.root, fault], capture_output=True, text=True, timeout=60)
        if r.returncode: return f'killed {r.returncode}' if r.returncode < 0 else f'exit {r.returncode}: {r.stderr.strip()[-300:]}'
        return r.stdout.strip()

    def staged(self, old=True):
        tree(self.tmp, 'new')
        if old: tree(self.dst, 'old')

    def assertStates(self, dst, tmp, old):
        self.assertEqual((state(self.dst), state(self.tmp), state(self.old)), (dst, tmp, old))

    def test_absent_destination(self):
        self.staged(old=False)
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')

    def test_existing_destination(self):
        self.staged()
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')

    def test_symlinked_destination_replaces_the_link_only(self):
        tree(self.root / 'target', 'old'); self.dst.symlink_to(self.root / 'target'); tree(self.tmp, 'new')
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')
        self.assertFalse(self.dst.is_symlink()); self.assertEqual(state(self.root / 'target'), 'old')

    def test_publication_failure_keeps_the_old_output(self):
        self.staged()
        self.assertEqual(self.swap('rename-fail'), 'OSError'); self.assertStates('old', 'new', 'absent')
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')

    def test_interrupt_at_publication_keeps_the_old_output(self):
        self.staged()
        self.assertEqual(self.swap('rename-int'), 'KeyboardInterrupt'); self.assertStates('old', 'new', 'absent')

    def test_interrupt_while_deleting_the_old_output(self):   # the new output is in; the old one's remains are deleted by the next swap
        self.staged()
        self.assertEqual(self.swap('unlink-int:5'), 'KeyboardInterrupt'); self.assertStates('new', 'absent', 'mixed')
        tree(self.tmp, 'new')
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')

    def test_cleanup_failure_is_reported_after_publication(self):
        self.staged()
        self.assertEqual(self.swap('unlink-fail:5'), 'PermissionError'); self.assertStates('new', 'absent', 'mixed')

    def test_interrupt_while_deleting_an_earlier_runs_aside_folder(self):
        self.staged(); tree(self.old, 'old')
        self.assertEqual(self.swap('unlink-int:2'), 'KeyboardInterrupt'); self.assertStates('old', 'new', 'mixed')
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')

    def test_kill_between_the_renames_then_rerun(self):   # the one state outside old / new: no output, the old one aside; a rerun publishes
        self.staged()
        self.assertEqual(self.swap('rename-kill'), 'killed -9'); self.assertStates('absent', 'new', 'old')
        self.assertEqual(self.swap(), 'ok'); self.assertStates('new', 'absent', 'absent')


if __name__ == '__main__':
    unittest.main()
