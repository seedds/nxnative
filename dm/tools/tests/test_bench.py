#!/usr/bin/env python3
# scripts/bench.py: the measured window of the host's per-frame rows (DM_NVN_TSV) and the percentiles taken from it, and the profiles' bench.tsv routes
import argparse, contextlib, importlib.util, io, json, os, signal, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('bench', REPO / 'scripts/bench.py'); bench = importlib.util.module_from_spec(spec); spec.loader.exec_module(bench)


class Bench(unittest.TestCase):
    def test_window_and_percentiles(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'frames.tsv'
            rows = ['# kind: nvn-per-frame', 'frame\tname\tvalue']
            for i in range(200):   # 100 frames before the window, 100 in it: 99 of 10 ms and one of 50 ms; the GPU time of the last frame in the window not known yet
                t = 10 + i * 0.01
                rows += [f'{i}\t@t_s\t{t:.3f}', f'{i}\t@frame_ms\t{50.0 if i == 150 else 10.0}', f'{i}\t@presented_ms\t{-1 if i == 160 else 8.333}']
                if i < 199: rows.append(f'{i}\t@gpu_ms\t{7.0 + (i % 10) * 0.1:.1f}')
                rows.append(f'{i}\tnvnQueueSubmitCommands\t3')   # call-count rows are not timing rows
            f.write_text('\n'.join(rows) + '\n')
            ft, shown, gpu, dropped, n = bench.frames(f, 11.0, 12.0)
            self.assertEqual(n, 100); self.assertEqual(len(ft), 100); self.assertEqual(dropped, 1); self.assertEqual(len(shown), 99); self.assertEqual(len(gpu), 99)
            s = bench.summary(ft, shown, gpu, dropped, n, 1.0, 1000 / 120)
            self.assertEqual(s['fps'], 100.0); self.assertEqual(s['frame_p50'], 10.0); self.assertEqual(s['frame_p99'], 10.0); self.assertEqual(s['frame_max'], 50.0)
            self.assertEqual(s['over_budget'], 1)   # only the 50 ms frame is longer than 1.5 × 8.33 ms

    def test_submit_metrics(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'frames.tsv'
            rows = ['# kind: nvn-per-frame', 'frame\tname\tvalue']
            for i in range(10):
                t = 10.0 + i * 0.1
                draws = 1000 if i < 5 else 2000
                cpu = 2.0 if i < 5 else 6.0   # 2.0 us/draw then 3.0 us/draw
                instr = 20_000_000 if i < 5 else 60_000_000   # 20k then 30k instr/draw
                rows += [f'{i}\t@t_s\t{t:.3f}', f'{i}\t@draws\t{draws}', f'{i}\t@sub_cpu_ms\t{cpu}', f'{i}\t@sub_instr\t{instr}']
            f.write_text('\n'.join(rows) + '\n')
            m = bench.submit_metrics(f, 10.5, 11.0)   # frames 5..9
            self.assertEqual(m['draws_p50'], 2000)
            self.assertEqual(m['sub_cpu_p50'], 6.0)
            self.assertEqual(m['sub_us_draw'], 3.0)
            self.assertEqual(m['sub_minstr_p50'], 60.0)
            self.assertEqual(m['sub_kinstr_draw'], 30.0)
            self.assertEqual(bench.submit_metrics(f, 20.0, 21.0), {})

    def test_percentile(self):
        xs = list(range(1, 101))
        self.assertEqual(bench.pct(xs, 50), 50); self.assertEqual(bench.pct(xs, 99), 99); self.assertEqual(bench.pct(xs, 100), 100); self.assertIsNone(bench.pct([], 50))

    def test_profile_routes(self):   # every bench.tsv parses: four columns at least, numbers in the window columns
        for t in (REPO / 'dm/games').glob('*/bench.tsv'):
            rs = bench.routes(t.parent.name)
            self.assertTrue(rs, t)
            for name, r in rs.items(): self.assertTrue(r['keys'] and r['secs'] > 0 and r['from'] >= 0, f'{t}: {name}')

    def test_optional_metrics_across_runs(self):
        runs = [{'frames': 2, 'fps': 60, 'gpu_p50': None}, {'frames': 2, 'fps': 58, 'gpu_p50': 7, 'game_speed': 1.0},
                {'frames': 2, 'fps': 62, 'gpu_p50': 9}, {'frames': 0, 'fps': 1, 'game_speed': 0.1}]
        for order in (runs, list(reversed(runs))):
            row = bench.table({'base': order}, ['fps', 'gpu_p50', 'game_speed', 'missing']).splitlines()[-1]
            self.assertEqual(row, '| base | 3 | 60 (58–62) | 8 (7–9) | 1 | – |')

    def test_cli_relative_paths(self):
        with mock.patch.dict(os.environ), tempfile.TemporaryDirectory() as d:
            os.environ.pop('DM_PSO_BUNDLE', None)   # exercise the default regardless of the caller's archive override
            root = Path(d); gid = '01007EF00011E000'; build = root / f'{gid}_build'
            (build / 'run/save').mkdir(parents=True); (build / 'run/save/sentinel').write_text('original')
            (build / 'extract/v0/RomFS').mkdir(parents=True); (build / 'pso/mac').mkdir(parents=True)   # the offline archive the Mac app bundles
            (root / 'variant/patched_v0').mkdir(parents=True)
            (root / 'variant/msl').mkdir(); (root / 'variant/pso').mkdir(); (root / 'variant/shmap.tsv').write_text('# synthetic\n')
            host = root / 'variant/host'
            host.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
paths = [sys.argv[1]] + [os.environ[k] for k in ('DM_ROOT', 'DM_ROMFS', 'DM_SAVE', 'DM_TEMP', 'DM_MSL_DIR', 'DM_NVN_SHMAP', 'DM_NVN_TSV', 'DM_CTL', 'DM_PSO_BUNDLE')]
assert all(pathlib.Path(p).is_absolute() for p in paths), paths
assert pathlib.Path(sys.argv[1]).is_dir()
assert pathlib.Path(os.environ['DM_MSL_DIR']).is_dir()
assert pathlib.Path(os.environ['DM_PSO_BUNDLE']) == pathlib.Path(os.environ['DM_ROOT']) / 'pso/mac'
assert pathlib.Path(os.environ['DM_NVN_SHMAP']).is_file()
archive = os.environ['DM_PSO_ARCHIVE']
assert archive == 'off' or pathlib.Path(archive).is_absolute() and pathlib.Path(archive).is_dir()
save = pathlib.Path(os.environ['DM_SAVE']) / 'sentinel'
assert save.read_text() == 'original'
save.write_text('child changed its copy')
pathlib.Path(os.environ['DM_NVN_TSV']).write_text('1\\t@t_s\\t0.01\\n1\\t@frame_ms\\t10\\n')
pathlib.Path('paths.json').write_text(json.dumps(paths))
''')
            host.chmod(0o755)
            route = {'keys': '0:A', 'from': 0, 'secs': 0.1, 'about': 'synthetic'}
            cwd = Path.cwd()
            try:
                os.chdir(root)
                variant = 'base:host=variant/host,patched=variant/patched_v0,msl=variant/msl,shmap=variant/shmap.tsv'
                for extra in (['--variant', variant + ',pso=variant/pso'], ['--variant', variant, '--pso-archive', 'variant/pso'],
                              ['--variant', variant + ',pso=off'], ['--variant', variant, '--pso-archive', 'off']):
                    with mock.patch.object(sys, 'argv', ['bench.py', build.name, '--out', 'relative-results', '--runs', '1', '--no-hud'] + extra), \
                         mock.patch.object(bench, 'routes', return_value={'test': route}), contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaises(SystemExit) as ex: bench.main()
                    self.assertEqual(ex.exception.code, 0, (root / 'relative-results/base-1/log.txt').read_text())
                    results = json.loads((root / 'relative-results/results.json').read_text())
                    self.assertEqual(results['results']['base'][0]['frames'], 1)
                    self.assertTrue((root / 'relative-results/results.md').is_file())
                    self.assertEqual((build / 'run/save/sentinel').read_text(), 'original')
            finally:
                os.chdir(cwd)

    def test_failed_launch_releases_lock(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); build = root / 'build'; (build / 'patched_v0').mkdir(parents=True); (build / 'extract/v0/RomFS').mkdir(parents=True)
            save = root / 'source-save'; save.mkdir(); lock = root / 'bench.lock'
            a = argparse.Namespace(save=save, pso_archive=None, fps=None, scale=None, env=[], no_hud=True, shots=None)
            with self.assertRaises(FileNotFoundError):
                bench.run_one(a, '01007EF00011E000', build, {'host': str(root / 'missing-host')}, {'keys': '0:A', 'from': 0, 'secs': 1}, root / 'out', lock)
            self.assertFalse(lock.exists(), 'a failed launch must not block the next benchmark')

    def test_cloud_saves_off(self):   # an inherited DM_SAVE_SYNC would sync the run's scratch save with the player's cloud save (docs/reference/services.md)
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); build = root / 'build'; (build / 'patched_v0').mkdir(parents=True); (build / 'extract/v0/RomFS').mkdir(parents=True)
            save = root / 'source-save'; save.mkdir()
            a = argparse.Namespace(save=save, pso_archive=None, fps=None, scale=None, env=[], no_hud=True, shots=None)
            seen = {}
            def popen(cmd, env, **kw): seen.update(env); raise FileNotFoundError(cmd[0])
            route = {'keys': '0:A', 'from': 0, 'secs': 1}
            with mock.patch.dict(os.environ, DM_SAVE_SYNC=str(root / 'cloud'), DM_SAVE_SYNC_CHOICE='cloud'), mock.patch.object(bench.subprocess, 'Popen', popen):
                with self.assertRaises(FileNotFoundError): bench.run_one(a, '01007EF00011E000', build, {'host': str(root / 'host')}, route, root / 'out', None)
                self.assertEqual(seen['DM_SAVE_SYNC'], '0'); self.assertNotIn('DM_SAVE_SYNC_CHOICE', seen)
                with contextlib.redirect_stderr(io.StringIO()):
                    for var, env in (({'host': str(root / 'host'), 'DM_SAVE_SYNC': str(root / 'cloud')}, []), ({'host': str(root / 'host')}, [f'DM_SAVE_SYNC={root}/cloud'])):
                        with self.assertRaises(SystemExit): bench.run_one(argparse.Namespace(**{**vars(a), 'env': env}), '01007EF00011E000', build, var, route, root / 'out', None)
                seen.clear()
                with self.assertRaises(FileNotFoundError): bench.run_one(argparse.Namespace(**{**vars(a), 'env': ['DM_SAVE_SYNC=0']}), '01007EF00011E000', build, {'host': str(root / 'host')}, route, root / 'out', None)
                self.assertEqual(seen['DM_SAVE_SYNC'], '0')


class Stop(unittest.TestCase):
    """bench.py stopped while a host runs (a real bench.py process, a fake host that writes its pid once the lock is taken): a front end's SIGTERM or an interrupt to bench.py
    alone stops the host, also one that ignores SIGTERM, and drops --lock; exit code 143 or 130"""
    GID = '01007EF00011E000'   # a profile with a bench.tsv: its first route, whose window outlasts each test
    BOUND = 20.0   # seconds bench.py may take to end (the host's grace period is 5 s)

    def setUp(self):
        d = tempfile.TemporaryDirectory(); self.addCleanup(d.cleanup)
        root = Path(d.name); self.build = root / f'{self.GID}_build'; self.lock = root / 'measure.lock'; self.marker = root / 'host.pid'
        for sub in ('patched_v0', 'extract/v0/RomFS', 'run/save'): (self.build / sub).mkdir(parents=True)
        self.host = root / 'host'; self.out = root / 'out'
        self.host.write_text(f'''#!{sys.executable}
import os, signal, sys
if os.environ.get('FAKE_HOST_IGNORE_TERM'): signal.signal(signal.SIGTERM, signal.SIG_IGN)
open({str(self.marker)!r} + '.tmp', 'w').write(str(os.getpid())); os.rename({str(self.marker)!r} + '.tmp', {str(self.marker)!r})
while True: signal.pause()
''')
        self.host.chmod(0o755)
        self.host_pid = 0
        self.addCleanup(self.kill_host)

    def kill_host(self):
        if self.host_pid:
            with contextlib.suppress(ProcessLookupError): os.kill(self.host_pid, signal.SIGKILL)

    def bench(self, *signals, ignore_term=False):
        env = dict(os.environ, FAKE_HOST_IGNORE_TERM='1' if ignore_term else '')
        p = subprocess.Popen([sys.executable, REPO / 'scripts/bench.py', self.build, '--runs', '1', '--no-hud', '--out', self.out, '--lock', self.lock,
                              '--variant', f'base:host={self.host}'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            end = time.monotonic() + self.BOUND
            while not self.marker.exists():
                self.assertIsNone(p.poll(), 'bench.py ended before the host started'); self.assertLess(time.monotonic(), end, 'the host did not start')
                time.sleep(0.05)
            self.host_pid = int(self.marker.read_text())
            self.assertTrue(self.lock.exists(), 'the lock is held while the host runs')
            t0 = time.monotonic()
            for sig in signals: p.send_signal(sig); time.sleep(0.3)
            p.wait(self.BOUND)
            elapsed = time.monotonic() - t0
        finally:
            if p.poll() is None: p.kill(); p.wait()
            if p.stderr: p.stderr.close()
        return p.returncode, elapsed

    def host_running(self):   # a zombie counts as gone: launchd reaps the orphan
        if not self.host_pid: return False
        try: os.kill(self.host_pid, 0)
        except ProcessLookupError: return False
        return subprocess.run(['ps', '-o', 'stat=', '-p', str(self.host_pid)], capture_output=True, text=True).stdout.strip()[:1] not in ('', 'Z')

    def assert_stopped(self, rc, want):
        end = time.monotonic() + 2
        while self.host_running() and time.monotonic() < end: time.sleep(0.05)
        self.assertFalse(self.host_running(), f'host {self.host_pid} still running after bench.py ended')
        self.assertFalse(self.lock.exists(), 'the measuring lock was left behind')
        self.assertEqual(rc, want)

    def test_sigterm(self):
        rc, _ = self.bench(signal.SIGTERM)
        self.assert_stopped(rc, 143)

    def test_interrupt(self):
        rc, _ = self.bench(signal.SIGINT)
        self.assert_stopped(rc, 130)

    def test_sigterm_host_ignores_sigterm(self):
        rc, _ = self.bench(signal.SIGTERM, ignore_term=True)
        self.assert_stopped(rc, 143)

    def test_second_sigterm_while_the_host_is_stopped(self):
        rc, elapsed = self.bench(signal.SIGTERM, signal.SIGTERM, ignore_term=True)
        self.assert_stopped(rc, 143)
        self.assertLess(elapsed, bench.HOST_GRACE, 'a second SIGTERM must escalate to SIGKILL instead of waiting out the grace period')

    def test_handler_given_back(self):   # main sets its SIGTERM/SIGINT handlers only while it runs (the tests above call it in a process of its own; this one in-process)
        def mine(sig, frame): pass
        self.addCleanup(signal.signal, signal.SIGTERM, signal.signal(signal.SIGTERM, mine))
        orig_int = signal.getsignal(signal.SIGINT)
        route = {'keys': '0:A', 'from': 0, 'secs': 0.1, 'about': 'synthetic'}   # a host run that ends by itself: the window and its two-second tail
        for argv in (['bench.py', str(self.build), '--route', 'none'], ['bench.py', str(self.build), '--runs', '1', '--no-hud', '--out', str(self.out), '--variant', f'base:host={self.host}']):
            with mock.patch.object(sys, 'argv', argv), mock.patch.object(bench, 'routes', return_value={'test': route}), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): bench.main()
            self.assertIs(signal.getsignal(signal.SIGTERM), mine)
            self.assertIs(signal.getsignal(signal.SIGINT), orig_int)
        self.host_pid = int(self.marker.read_text()); self.assertFalse(self.host_running())

    def test_signal_during_popen_before_assignment(self):   # SIGTERM/SIGINT arriving after the child starts but before `p = Popen(...)` binds still stops the host and drops the lock
        real_popen = subprocess.Popen
        route = {'keys': '0:A', 'from': 0, 'secs': 60, 'about': 'synthetic'}
        for sig, want in ((signal.SIGTERM, 143), (signal.SIGINT, 130)):
            if self.marker.exists(): self.marker.unlink()
            self.host_pid = 0
            def popen_with_signal(*args, _sig=sig, **kw):
                proc = real_popen(*args, **kw)
                end = time.monotonic() + self.BOUND
                while not self.marker.exists():
                    self.assertLess(time.monotonic(), end, 'the host did not start')
                    time.sleep(0.02)
                self.host_pid = int(self.marker.read_text())
                os.kill(os.getpid(), _sig)   # fires inside Popen before the caller binds `p`
                return proc
            argv = ['bench.py', str(self.build), '--runs', '1', '--no-hud', '--out', str(self.out), '--lock', str(self.lock), '--variant', f'base:host={self.host}']
            with mock.patch.object(sys, 'argv', argv), mock.patch.object(bench, 'routes', return_value={'test': route}), \
                 mock.patch.object(bench.subprocess, 'Popen', popen_with_signal), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as ex: bench.main()
            self.assert_stopped(ex.exception.code, want)

    def test_stop_host_repeated_signal_during_kill_wait(self):   # a signal arriving during SIGKILL/wait must not skip reaping the child
        kills = 0
        waits = 0
        class FakeProc:
            def poll(self): return None
            def send_signal(self, sig): pass
            def kill(self):
                nonlocal kills
                kills += 1
            def wait(self, timeout=None):
                nonlocal waits
                waits += 1
                if timeout is not None: raise bench.Stopped()
                if waits == 2: raise KeyboardInterrupt()
                return -9
        with self.assertRaises(KeyboardInterrupt): bench.stop_host(FakeProc())
        self.assertEqual(kills, 2)
        self.assertEqual(waits, 3)

    def test_sigterm_while_waiting_on_other_lock_keeps_it(self):   # stopping while waiting on another live process's lock must exit 143 and leave that lock intact
        self.lock.write_text(f'other pid {os.getpid()}')
        p = subprocess.Popen([sys.executable, REPO / 'scripts/bench.py', self.build, '--runs', '1', '--no-hud', '--out', self.out, '--lock', self.lock,
                              '--variant', f'base:host={self.host}'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(0.4)
            self.assertIsNone(p.poll(), 'bench.py should wait on another live holder')
            self.assertFalse(self.marker.exists(), 'host must not start while lock is held')
            p.send_signal(signal.SIGTERM)
            p.wait(self.BOUND)
        finally:
            if p.poll() is None: p.kill(); p.wait()
            if p.stderr: p.stderr.close()
        self.assertEqual(p.returncode, 143)
        self.assertTrue(self.lock.exists(), 'another holder\'s lock must not be removed')
        self.assertEqual(self.lock.read_text(), f'other pid {os.getpid()}')
        self.assertFalse(self.marker.exists())


if __name__ == '__main__':
    unittest.main()
