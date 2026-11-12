"""proc_group.py and its two callers, the NSP script's run and regress.py's run_heavy, on real process trees (fixtures/proc_tree.py: a wrapper and a child of it in the
group the supervisor owns, each ready before the test acts): a cancellation (an interrupt raised in the progress callback, a SIGTERM, a SIGHUP, an error writing the log, a second
interrupt, a paused job) stops the whole group within its grace period, a child left running after the wrapper's normal exit (holding the output or not) is stopped and
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


def proc_status(pid):   # a process's state from its proc_bsdinfo: gone, zombie, stopped or running
    info = ctypes.create_string_buffer(136)
    if proc_group._libproc.proc_pidinfo(pid, 3, 0, info, 136) != 136: return 'gone'
    return {5: 'zombie', 4: 'stopped'}.get(int.from_bytes(info.raw[4:8], 'little'), 'running')


if __name__ == '__main__':
    unittest.main()
