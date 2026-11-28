"""proc_group.py and its callers (the NSP script's run and rewrite, regress.py's run_heavy and modules_for) on real process trees (fixtures/proc_tree.py: a wrapper and a child of it in the
group the supervisor owns, each ready before the test acts): a cancellation (an interrupt raised in the progress callback, a SIGTERM, a SIGHUP, an error writing the log, a second
interrupt, a paused job, a failed sibling rewrite) stops every group within its grace period, a child left running after the wrapper's normal exit (holding the output or not) is stopped and
fails the step, exit codes in ok pass and others fail, and cli gives SIGTERM's and SIGHUP's handlers back. Every check is that no process of the group is left, not only the wrapper.
No game files needed; a temporary folder per test."""
import contextlib
import ctypes
import importlib.util
import io
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace as NS
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
TREE = Path(__file__).resolve().parent / 'fixtures/proc_tree.py'
sys.path.insert(0, str(REPO / 'dm/tools'))
import proc_group   # noqa: E402


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


nsp = load('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
regress = load('regress', REPO / 'scripts/regress.py')
BOUND = 10.0   # seconds any one stop may take here (the grace periods below are shorter)


class Tree(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory(); self.addCleanup(d.cleanup)
        self.dir = Path(d.name); self.marker = self.dir / 'marker'; self.log = self.dir / 'logs/tool.log'
        self.pgids = []
        self.addCleanup(self.kill_leftovers)
        for cm in (mock.patch.object(proc_group, 'GRACE', 2.0), mock.patch.object(proc_group, 'DRAIN', 0.5)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def kill_leftovers(self):   # teardown whatever a failed test left (only the groups it started)
        for pg in self.pgids:
            with contextlib.suppress(ProcessLookupError, PermissionError): os.killpg(pg, signal.SIGKILL)   # EPERM: only zombies left

    def cmd(self, *opts, fifo=False):
        return [sys.executable, TREE, self.marker, *(['--fifo', self.fifo()] if fifo else []), *opts]

    def fifo(self):
        f = self.dir / 'ready'
        if not f.exists(): os.mkfifo(f)
        return f

    def wait_ready(self):   # the wrapper's READY line through the fifo → (wrapper pid, child pid)
        fd = os.open(self.fifo(), os.O_RDONLY | os.O_NONBLOCK)
        try:
            data, end = b'', time.monotonic() + BOUND
            while not data.endswith(b'\n'):
                r, _, _ = select.select([fd], [], [], max(0.0, end - time.monotonic()))
                self.assertTrue(r, 'the process tree did not get ready')
                data += os.read(fd, 100)
        finally: os.close(fd)
        return self.ready(data.decode())

    def ready(self, line):
        _, w, c = line.split(); w, c = int(w), int(c)
        self.pgids.append(w)
        return w, c

    def marks(self): return self.marker.read_text().split()[1:]

    def assert_gone(self, pgid, child):   # every process of the group gone, the child too wherever it is (a zombie writes nothing)
        self.assertGreater(min(pgid, child), 0, 'the process tree never got ready')
        self.assertEqual(proc_group.members(pgid), [])
        self.assertIn(proc_status(child), ('gone', 'zombie'))


class Run(Tree):
    """The NSP script's run"""
    def setUp(self):
        super().setUp()
        self.out = io.StringIO()
        for cm in (mock.patch.object(nsp, 'PROG', nsp.Progress('off')), contextlib.redirect_stdout(self.out)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def run_tree(self, *opts, stop=None, **kw):   # run with a progress callback that raises stop at READY; → (result or exception, seconds, (wrapper, child))
        got = {}
        def on_line(line):
            if line.startswith('READY '):
                got['ids'] = self.ready(line)
                if stop: stop()
        t0 = time.monotonic()
        try: r = nsp.run(self.cmd(*opts), self.log, on_line=on_line, **kw)
        except BaseException as e: r = e   # noqa: BLE001 (each test checks which)
        return r, time.monotonic() - t0, got.get('ids', (-1, -1))   # -1: no READY line (assert_gone fails on it)

    def test_interrupt_in_progress_callback_stops_the_group(self):   # Spyder's interrupt: it reaches the script alone
        def interrupt(): raise KeyboardInterrupt
        r, secs, (w, c) = self.run_tree(stop=interrupt)
        self.assertIsInstance(r, KeyboardInterrupt)
        self.assertLess(secs, BOUND)
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['INT'])   # the child got the interrupt, as a terminal's Ctrl-C gives it

    def test_ignored_signals_end_in_sigkill(self):
        def interrupt(): raise KeyboardInterrupt
        r, secs, (w, c) = self.run_tree('--child', 'ignore', stop=interrupt)
        self.assertIsInstance(r, KeyboardInterrupt)
        self.assertGreaterEqual(secs, proc_group.GRACE)
        self.assertLess(secs, proc_group.GRACE + proc_group.REAP + BOUND / 2)
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), [])

    def test_sigterm_stops_the_group_with_sigterm(self):
        r, secs, (w, c) = self.run_tree(stop=lambda: nsp._sigterm(signal.SIGTERM, None))
        self.assertIsInstance(r, nsp.Stopped)
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['TERM'])

    def test_a_log_that_cannot_be_written_stops_the_group(self):
        real = open
        class Full(io.StringIO):
            def write(self, s):
                if s.startswith('READY '): ids.append(s); raise OSError(28, 'No space left on device')
                return super().write(s)
        ids = []
        with mock.patch.object(nsp, 'open', create=True, side_effect=lambda p, *a, **k: Full() if Path(p) == self.log else real(p, *a, **k)):
            r, secs, _ = self.run_tree()
        self.assertIsInstance(r, OSError)
        w, c = self.ready(ids[0])
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['TERM'])

    def test_normal_completion(self):
        r, _, (w, c) = self.run_tree('--child', 'exit', '--wrapper', 'join:0')
        self.assertEqual(r, 0)
        self.assertEqual(self.log.read_text().split()[-1], 'done')
        self.assert_gone(w, c)

    def test_exit_codes(self):
        r, _, _ = self.run_tree('--child', 'exit', '--wrapper', 'join:5', ok=(5,))
        self.assertEqual(r, 0)
        r, _, _ = self.run_tree('--child', 'exit', '--wrapper', 'join:7', ok=(5,))
        self.assertIsInstance(r, nsp.BuildError); self.assertIn('exit code 7', str(r))
        r, _, _ = self.run_tree('--child', 'exit', '--wrapper', 'join:7', fatal=False)
        self.assertEqual(r, 7)

    def test_a_child_holding_the_output_after_the_wrapper_exits(self):
        r, secs, (w, c) = self.run_tree('--wrapper', 'exit:0')
        self.assertIsInstance(r, nsp.BuildError); self.assertIn('still ran', str(r)); self.assertIn(f'pid {c}', str(r))
        self.assertLess(secs, proc_group.DRAIN + BOUND)
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['TERM'])

    def test_a_child_left_running_without_the_output(self):   # an exit code in ok does not make it pass
        r, _, (w, c) = self.run_tree('--wrapper', 'exit:5', '--child-out', 'null', ok=(5,))
        self.assertIsInstance(r, nsp.BuildError); self.assertIn(f'pid {c}', str(r))
        self.assert_gone(w, c)
        r, _, (w, c) = self.run_tree('--wrapper', 'exit:0', '--child-out', 'null', fatal=False)
        self.assertEqual(r, 1); self.assertIn('[RED] ', self.out.getvalue())
        self.assert_gone(w, c)


class Cli(Tree):
    """cli's SIGTERM and SIGHUP handlers: set while it runs, the caller's again after it, whichever way it ends"""
    def setUp(self):
        super().setUp()
        for cm in (mock.patch.object(nsp, 'PROG', nsp.Progress('off')), contextlib.redirect_stdout(io.StringIO()), mock.patch.object(nsp, 'parse_args')):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        self.mine = lambda *a: None
        for sig in nsp.STOPS: self.addCleanup(signal.signal, sig, signal.signal(sig, self.mine))

    def cli(self, main):
        with mock.patch.object(nsp, 'main', side_effect=main): code = nsp.cli([])
        for sig in nsp.STOPS: self.assertIs(signal.getsignal(sig), self.mine)
        return code

    def test_success_failure_interrupt(self):
        def raise_(e):
            def f(_): raise e
            return f
        self.assertEqual(self.cli(lambda _: [self.assertIs(signal.getsignal(sig), nsp._sigterm) for sig in nsp.STOPS] and None), 0)
        self.assertEqual(self.cli(raise_(nsp.BuildError('no'))), 1)
        self.assertEqual(self.cli(raise_(KeyboardInterrupt())), 130)
        with self.assertRaises(ValueError): self.cli(raise_(ValueError('unexpected')))
        self.assertIs(signal.getsignal(signal.SIGTERM), self.mine)

    def test_sigterm_during_a_tool(self):   # a front end's cancel: a real SIGTERM to the script while a tool's group runs
        ids = []
        def main(_):
            def on_line(line):
                if line.startswith('READY '): ids.append(self.ready(line)); os.kill(os.getpid(), signal.SIGTERM)
            nsp.run(self.cmd(), self.log, on_line=on_line)
        self.assertEqual(self.cli(main), 143)
        self.assert_gone(*ids[0])
        self.assertEqual(self.marks(), ['TERM'])

    def test_sighup_during_a_tool(self):   # the terminal closed: the shell's SIGHUP reaches the script's job, not the tool's group (a session of its own)
        ids = []
        def main(_):
            def on_line(line):
                if line.startswith('READY '): ids.append(self.ready(line)); os.kill(os.getpid(), signal.SIGHUP)
            nsp.run(self.cmd(), self.log, on_line=on_line)
        self.assertEqual(self.cli(main), 129)
        self.assert_gone(*ids[0])
        self.assertEqual(self.marks(), ['TERM'])


class Heavy(Tree):
    """regress.py's run_heavy: the output goes to a file, the group is paused while the measuring lock is held"""
    def setUp(self):
        super().setUp()
        for cm in (mock.patch.object(regress, 'say'), mock.patch.object(regress, 'HEAVY_POLL', 0.01)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        self.log.parent.mkdir()
        self.a = NS(pause_lock=self.dir / 'lock')

    def heavy(self, *opts, holders):   # holders: what lock_holder returns, call by call (a callable is called; the first call waits for READY)
        calls, ids = iter(holders), []
        def holder(_):
            if not ids: ids.append(self.wait_ready())
            h = next(calls, None)
            return h() if callable(h) else h
        with mock.patch.object(regress.bench, 'lock_holder', side_effect=holder):
            try: r = regress.run_heavy(self.cmd(*opts, fifo=True), self.log, self.dir, None, self.a, 'job')
            except BaseException as e: r = e   # noqa: BLE001
        return r, ids[0] if ids else (-1, -1)

    def test_an_interrupt_while_paused_resumes_then_stops(self):   # a paused group gets the interrupt only once resumed: its handler marks it
        seen = []
        def interrupt():
            seen.append([proc_status(p) for p, _ in proc_group.members(self.pgids[-1])])
            raise KeyboardInterrupt
        r, (w, c) = self.heavy(holders=[0, interrupt])   # pid 0: an owner the lock file does not name still pauses the job
        self.assertIsInstance(r, KeyboardInterrupt)
        self.assertEqual(seen, [['stopped', 'stopped']])
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['INT'])

    def test_paused_seconds(self):
        clock = iter([100.0, 130.0])
        with mock.patch.object(regress, 'time', NS(time=lambda: next(clock))):
            r, (w, c) = self.heavy('--child', 'mark+cont', '--wrapper', 'join:0', holders=[4242, None])
        self.assertEqual(r, (0, 30))
        self.assertEqual(self.marks(), ['CONT'])
        self.assert_gone(w, c)

    def test_exit_status_and_stragglers(self):
        r, _ = self.heavy('--child', 'exit', '--wrapper', 'join:7', holders=[])
        self.assertEqual(r, (7, 0))
        r, (w, c) = self.heavy('--wrapper', 'exit:0', holders=[])
        self.assertEqual(r, (1, 0))
        self.assert_gone(w, c)
        self.assertIn(f'pid {c}', self.log.read_text().splitlines()[-1])


class Supervisor(Tree):
    """proc_group.Group on its own"""
    def test_a_second_interrupt_cuts_the_grace_period(self):
        with mock.patch.object(proc_group, 'GRACE', 60.0):
            g = proc_group.Group(self.cmd('--child', 'ignore', fifo=True), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); self.pgids.append(g.pgid)
            w, c = self.wait_ready()
            sleep, n = time.sleep, []
            def second(s):
                n.append(s)
                if len(n) == 3: raise KeyboardInterrupt
                sleep(s)
            t0 = time.monotonic()
            with mock.patch.object(proc_group.time, 'sleep', side_effect=second), self.assertRaises(KeyboardInterrupt):
                g.stop(signal.SIGINT)
        self.assertLess(time.monotonic() - t0, BOUND)
        self.assert_gone(w, c)

    def test_an_error_after_the_leader_exited(self):   # the direct child gone, its child still running: an error stops it all the same
        g = proc_group.Group(self.cmd('--wrapper', 'exit:0', '--child-out', 'null', fifo=True), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); self.pgids.append(g.pgid)
        with self.assertRaises(RuntimeError), g:
            w, c = self.wait_ready(); g.p.wait()
            self.assertEqual([p for p, _ in proc_group.members(g.pgid)], [c])
            raise RuntimeError
        self.assert_gone(w, c)
        self.assertEqual(self.marks(), ['TERM'])
        self.assertEqual(g.stragglers, [])   # a stop, not a straggler: the caller reports its own error

    def test_stop_all_stops_multiple_groups_in_one_grace_period(self):
        with mock.patch.object(proc_group, 'GRACE', 1.0):
            groups, pairs = [], []
            for i in range(2):
                fifo = self.dir / f'ready_{i}'; os.mkfifo(fifo)
                g = proc_group.Group([sys.executable, TREE, self.dir / f'marker_{i}', '--fifo', fifo, '--child', 'ignore'],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.pgids.append(g.pgid); groups.append(g)
                with mock.patch.object(self, 'fifo', return_value=fifo): pairs.append(self.wait_ready())
            t0 = time.monotonic()
            proc_group.stop_all(groups, signal.SIGTERM)
            secs = time.monotonic() - t0
        self.assertGreaterEqual(secs, 1.0)
        self.assertLess(secs, 1.9)   # one shared grace window, not one per group
        for w, c in pairs: self.assert_gone(w, c)

    def test_stop_all_second_interrupt_still_reaps_every_group(self):
        with mock.patch.object(proc_group, 'GRACE', 60.0):
            groups, pairs = [], []
            for i in range(2):
                fifo = self.dir / f'ready_int_{i}'; os.mkfifo(fifo)
                g = proc_group.Group([sys.executable, TREE, self.dir / f'marker_int_{i}', '--fifo', fifo, '--child', 'ignore'],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.pgids.append(g.pgid); groups.append(g)
                with mock.patch.object(self, 'fifo', return_value=fifo): pairs.append(self.wait_ready())
            sleep, n = time.sleep, []
            def second(s):
                n.append(s)
                if len(n) == 3: raise KeyboardInterrupt
                sleep(s)
            t0 = time.monotonic()
            with mock.patch.object(proc_group.time, 'sleep', side_effect=second), self.assertRaises(KeyboardInterrupt):
                proc_group.stop_all(groups, signal.SIGINT)
        self.assertLess(time.monotonic() - t0, BOUND)
        for w, c in pairs: self.assert_gone(w, c)


FAKE_REWRITER = '''
import os, signal, sys, time
from pathlib import Path
src, out, mod = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
mode = os.environ.get('FAKE_MODE', 'ok')
if mode == 'fail_with_sibling_worker':
    if mod == 'slow':
        pid = os.fork()
        if pid == 0:
            if os.environ.get('WORKER_IGNORE') == '1': signal.signal(signal.SIGTERM, signal.SIG_IGN)
            (out / 'worker_pids').write_text(f'{os.getppid()} {os.getpid()}\\n')
            time.sleep(1.0)
            (out / 'late_from_worker').write_text('late')
            os._exit(0)
        time.sleep(30)
    else:
        end = time.monotonic() + 5
        while not (out / 'worker_pids').is_file() and time.monotonic() < end: time.sleep(0.01)
        sys.exit(1)
elif mode == 'straggler':
    pid = os.fork()
    if pid == 0:
        (out / 'straggler_pid').write_text(f'{os.getppid()} {os.getpid()}\\n')
        time.sleep(30)
        os._exit(0)
    end = time.monotonic() + 5
    while not (out / 'straggler_pid').is_file() and time.monotonic() < end: time.sleep(0.01)
    sys.exit(0)
for ext in ('patched.bin', 'fixups.tsv', 'gates.tsv'):
    (out / f'{mod}.{ext}').write_bytes(b'ok')
'''


class ParallelRewrite(Tree):
    """regress.modules_for and nsp.rewrite: a failed or straggling rewrite stops sibling process groups and closes every log"""
    def setUp(self):
        super().setUp()
        for mod in (proc_group, regress.proc_group):
            for attr, val in (('GRACE', 0.4), ('DRAIN', 0.3)):
                cm = mock.patch.object(mod, attr, val)
                cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def _regress_fixture(self, modules='slow main', env=None):
        tree_dir = self.dir / 'tree'; (tree_dir / 'dm/tools').mkdir(parents=True, exist_ok=True)
        (tree_dir / 'dm/tools/patch_nso.py').write_text(FAKE_REWRITER)
        build = self.dir / 'build'; exefs = build / 'extract/v0/ExeFS'; exefs.mkdir(parents=True, exist_ok=True)
        for m in modules.split(): (exefs / m).write_bytes(m.encode())
        base_env = {'DM_GAME': 'game'} | (env or {})
        g = NS(id='game', folder=self.dir / 'nsp', build=build, ver='v0', patched=build / 'patched_v0', derived=False,
               env=lambda tree: dict(base_env))
        tree = NS(id='tree', path=tree_dir, py=lambda code, e: modules)
        return tree, g

    def test_regress_modules_for_failure_reaps_sibling_group_closes_logs_and_retries(self):
        real_open = open
        for ignore in ('0', '1'):
            with self.subTest(worker_ignore=ignore):
                tree, g = self._regress_fixture(env={'FAKE_MODE': 'fail_with_sibling_worker', 'WORKER_IGNORE': ignore})
                opened = []
                def track_open(path, *a, _opened=opened, **k):
                    fh = real_open(path, *a, **k)
                    if Path(path).name.startswith('rewrite_'): _opened.append(fh)
                    return fh
                with mock.patch.object(regress, 'rewriter_key', return_value=f'rev-{ignore}'), mock.patch.object(regress, 'say'), \
                     mock.patch.object(regress, 'open', create=True, side_effect=track_open):
                    with self.assertRaises(regress.HarnessError):
                        regress.modules_for(tree, g, True, 2)
                tmp = next((self.dir / 'nsp/game-work/regress/patched').glob('*/*.partial.*'))
                sib, worker = (int(x) for x in (tmp / 'worker_pids').read_text().split())
                self.pgids.append(sib)
                self.assert_gone(sib, worker)
                self.assertTrue(opened and all(f.closed for f in opened))
                self.assertFalse((tmp / 'late_from_worker').exists())
                time.sleep(0.9)
                self.assertFalse((tmp / 'late_from_worker').exists())

                # A retry in the same process reuses the same .partial.<pid> path cleanly.
                tree_ok, g_ok = self._regress_fixture(env={'FAKE_MODE': 'ok'})
                with mock.patch.object(regress, 'rewriter_key', return_value=f'rev-{ignore}'), mock.patch.object(regress, 'say'):
                    dst, _ = regress.modules_for(tree_ok, g_ok, True, 2)
                self.assertTrue((dst / '.done').is_file())
                self.assertTrue((dst / 'slow.patched.bin').is_file())

    def test_regress_modules_for_straggler_worker_fails_rewrite(self):
        tree, g = self._regress_fixture(modules='main', env={'FAKE_MODE': 'straggler'})
        with mock.patch.object(regress, 'rewriter_key', return_value='rev-strag'), mock.patch.object(regress, 'say'):
            with self.assertRaises(regress.HarnessError):
                regress.modules_for(tree, g, True, 1)
        tmp = next((self.dir / 'nsp/game-work/regress/patched').glob('*/*.partial.*'))
        parent, worker = (int(x) for x in (tmp / 'straggler_pid').read_text().split())
        self.pgids.append(parent)
        self.assert_gone(parent, worker)
        self.assertIn('[RED] patch_nso.py exited', (tmp / 'rewrite_main.log').read_text())

    def test_nsp_rewrite_failure_reaps_sibling_group_and_closes_logs(self):
        repo = self.dir / 'repo'; (repo / 'dm/tools').mkdir(parents=True)
        (repo / 'dm/tools/patch_nso.py').write_text(FAKE_REWRITER)
        exe = self.dir / 'exe'; exe.mkdir()
        for m in ('slow', 'main'): (exe / m).write_bytes(m.encode())
        out, logs = self.dir / 'patched_v0', self.dir / 'logs'
        g = NS(modules_in=lambda folder: ['slow', 'main'])
        opened, real_open = [], open
        def track_open(path, *a, **k):
            fh = real_open(path, *a, **k)
            if Path(path).name.startswith('rewrite_'): opened.append(fh)
            return fh
        with mock.patch.object(nsp, 'PROG', nsp.Progress('off')), mock.patch.object(nsp, 'collect_nros', return_value={}), \
             mock.patch.object(nsp, 'patch_files', return_value=[]), mock.patch.object(nsp, 'open', create=True, side_effect=track_open), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(nsp.BuildError):
                nsp.rewrite(Path(sys.executable), repo, exe, out, {'FAKE_MODE': 'fail_with_sibling_worker', 'WORKER_IGNORE': '1'}, logs, g)
        partial = out.with_name(out.name + '.partial')
        sib, worker = (int(x) for x in (partial / 'worker_pids').read_text().split())
        self.pgids.append(sib)
        self.assert_gone(sib, worker)
        self.assertTrue(opened and all(f.closed for f in opened))
        self.assertFalse((partial / 'late_from_worker').exists())
        time.sleep(0.9)
        self.assertFalse((partial / 'late_from_worker').exists())


def proc_status(pid):   # a process's state from its proc_bsdinfo: gone, zombie, stopped or running
    info = ctypes.create_string_buffer(136)
    if proc_group._libproc.proc_pidinfo(pid, 3, 0, info, 136) != 136: return 'gone'
    return {5: 'zombie', 4: 'stopped'}.get(int.from_bytes(info.raw[4:8], 'little'), 'running')


if __name__ == '__main__':
    unittest.main()
