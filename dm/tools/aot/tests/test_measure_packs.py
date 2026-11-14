#!/usr/bin/env python3
"""measure_packs.py against stand-in nxaot and nxaot_host scripts (written into a temporary folder): the alternation order, the statistics
(fastest of N, median, change), the verdicts for a clear gain, a clear loss and noise, identical packs, a differing output, a failing run and
the unpinned note. No Docker, no nxaot build.

  python3 -m unittest dm/tools/aot/tests/test_measure_packs.py
"""
import importlib.util
import os
import statistics
import subprocess
import sys
import tempfile
import textwrap
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'measure_packs.py')
spec = importlib.util.spec_from_file_location('measure_packs', TOOL)
mp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mp)

# The stand-in nxaot: `build <module> --shared <so> [--fake-speed S] [--fake-out X] [--fake-text N]`; FAKE_SPEED in the environment overrides
# --fake-speed. It writes a small ELF64 whose .text size is N and whose bytes hold the speed, the output and the soname (the file name), as a
# real pack's bytes depend on its soname.
FAKE_NXAOT = r'''
import os, struct, sys
a = sys.argv[1:]
assert a[0] == 'build', a
so = a[a.index('--shared') + 1]
def opt(name, default):
    return a[a.index(name) + 1] if name in a else default
speed = os.environ.get('FAKE_SPEED', opt('--fake-speed', '0.010'))
out = opt('--fake-out', 'same')
text = int(opt('--fake-text', '4096'))
payload = f'SPEED={speed};OUT={out};SONAME={os.path.basename(so)};'.encode()
shstr = b'\0.text\0.shstrtab\0'
body = payload + shstr
shoff = 64 + len(body)
hdr = b'\x7fELF' + bytes([2, 1, 1]) + bytes(9)
hdr += struct.pack('<HHIQQQIHHHHHH', 3, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 3, 2)
null = bytes(64)
text_sh = struct.pack('<IIQQQQIIQQ', 1, 1, 6, 0, 64, text, 0, 0, 16, 0)
str_sh = struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, 64 + len(payload), len(shstr), 0, 0, 1, 0)
with open(so, 'wb') as f:
    f.write(hdr + body + null + text_sh + str_sh)
print('aot: fake build', so)
'''

# The stand-in nxaot_host: `<so> <module> --repeat N [--set k=v]...`. It logs each call (variant, file) to FAKE_LOG, prints the pack's output
# line and PASS, exits FAKE_EXIT_<OUT> when that is set, and reports a time: speed x the layout's factor (1 + 0.01 x (the file name's length
# mod 3)) x a jitter that cycles through 1.00, 1.02, 1.04 over the calls of each variant (a counter file), the fastest of the N repeats.
FAKE_HOST = r'''
import os, re, sys
so, module = sys.argv[1], sys.argv[2]
n = int(sys.argv[sys.argv.index('--repeat') + 1])
data = open(so, 'rb').read().decode('latin-1')
speed = float(re.search(r'SPEED=([^;]+);', data).group(1))
out = re.search(r'OUT=([^;]+);', data).group(1)
name = os.path.basename(so)
variant = os.path.basename(os.path.dirname(so))
factor = 1 + 0.01 * (len(name) % 3)
counter = os.path.join(os.environ['FAKE_DIR'], 'counter.' + variant)
c = int(open(counter).read()) if os.path.exists(counter) else 0
times = []
for i in range(n):
    times.append(speed * factor * (1.00, 1.02, 1.04)[(c + i) % 3])
open(counter, 'w').write(str(c + n))
with open(os.environ['FAKE_LOG'], 'a') as log:
    log.write(f'{variant} {name}\n')
print('guest says', out)
print('PASS 1')
code = int(os.environ.get('FAKE_EXIT_' + out, '0'))
sys.stderr.write(f'nxaot_host: {module}: exited after {times[0]:.6f} s (fastest of {n}: {min(times):.6f} s), 2 lines, 0 dispatcher misses\n')
sys.exit(code)
'''


def read_lines(path):
    with open(path) as f:
        return f.read().splitlines()


class MeasurePacks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.nxaot = self.script('nxaot', FAKE_NXAOT)
        self.host = self.script('nxaot_host', FAKE_HOST)
        self.module = os.path.join(self.dir, 'prog.nro')
        with open(self.module, 'wb') as f:
            f.write(b'NRO0')
        self.work = os.path.join(self.dir, 'work')
        self.log = os.path.join(self.dir, 'calls.log')

    def tearDown(self):
        self.tmp.cleanup()

    def script(self, name, body):
        path = os.path.join(self.dir, name)
        with open(path, 'w') as f:
            f.write(f'#!{sys.executable}\n' + textwrap.dedent(body))
        os.chmod(path, 0o755)
        return path

    def run_tool(self, *args, env=None, layouts=3, rounds=4, repeats=3):
        e = dict(os.environ, FAKE_DIR=self.dir, FAKE_LOG=self.log)
        e.update(env or {})
        cmd = [sys.executable, TOOL, '--a-nxaot', self.nxaot, '--host', self.host, '--work', self.work, '--layouts', str(layouts),
               '--rounds', str(rounds), '--repeats', str(repeats)] + list(args) + [self.module]
        return subprocess.run(cmd, capture_output=True, text=True, env=e)

    def samples(self):
        rows = []
        for line in read_lines(os.path.join(self.work, 'samples.tsv')):
            if not line.startswith('#'):
                f = line.rstrip('\n').split('\t')
                rows.append({'round': int(f[1]), 'order': int(f[2]), 'layout': int(f[3]), 'file': f[4], 'variant': f[5], 'repeat': f[6],
                             'seconds': float(f[7])})
        return rows

    def verdict(self, out):
        line = next(l for l in out.splitlines() if l.startswith('verdict prog:'))
        return line.split(': ', 1)[1].split(' (')[0]

    # The statistics, directly
    def test_statistics(self):
        self.assertEqual(mp.fastest([3.0, 1.0, 2.0]), 1.0)
        self.assertEqual(mp.median([5.0, 1.0, 3.0]), 3.0)
        self.assertEqual(mp.median([4.0, 1.0, 3.0, 2.0]), 2.5)
        self.assertAlmostEqual(mp.change_pct(100.0, 90.0), -10.0)
        self.assertAlmostEqual(mp.spread_pct([99.0, 100.0, 101.0]), 2.0)
        self.assertEqual(mp.spread_pct([5.0]), 0.0)
        s = {('a', 0): [10.0, 11.0, 10.0], ('a', 1): [10.2, 10.2, 10.4], ('b', 0): [9.0, 9.0, 9.1], ('b', 1): [9.2, 9.3, 9.2]}
        r = mp.summarize(s, 2)
        self.assertEqual(r['per_layout']['a'], [10.0, 10.2])
        self.assertAlmostEqual(r['a'], 10.1)
        self.assertAlmostEqual(r['b'], 9.1)
        self.assertAlmostEqual(r['change'], (9.1 - 10.1) / 10.1 * 100)
        self.assertEqual(r['verdict'], 'faster')
        # one layout where B is not faster: within noise whatever the change
        s[('b', 1)] = [10.3, 10.3, 10.3]
        self.assertEqual(mp.summarize(s, 2)['verdict'], 'within noise')
        one = {('a', 0): [1.0], ('b', 0): [2.0]}
        self.assertEqual(mp.summarize(one, 1)['verdict'], 'no noise estimate')

    # A clear gain, through the processes: alternation, fastest of N, medians, the table
    def test_gain_alternation_and_statistics(self):
        r = self.run_tool('--a-args=--fake-speed 0.010 --fake-text 4096', '--b-args=--fake-speed 0.008 --fake-text 4000', '--repeat-mode', 'process')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.verdict(r.stdout), 'faster')
        # the order of the calls: per round, per layout, A then B; N processes each
        calls = [l.split() for l in read_lines(self.log)]
        self.assertEqual(len(calls), 4 * 3 * 2 * 3)
        expected = []
        for _ in range(4):
            for l in range(3):
                for v in 'ab':
                    expected += [[v, f'prog.L{l}' + 'x' * l + '.so']] * 3
        self.assertEqual(calls, expected)
        rows = self.samples()
        self.assertEqual([x['order'] for x in rows], list(range(1, len(rows) + 1)))
        # fastest of N: each (round, layout, variant) group of 3 processes has the jitter's three steps; its fastest is speed x factor
        groups = {}
        for x in rows:
            groups.setdefault((x['variant'], x['layout'], x['round']), []).append(x['seconds'])
        for (v, l, _), ts in groups.items():
            self.assertEqual(len(ts), 3)
            factor = 1 + 0.01 * (len(f'prog.L{l}' + 'x' * l + '.so') % 3)
            self.assertAlmostEqual(min(ts), (0.010 if v == 'a' else 0.008) * factor, places=6)
        # the medians and the change, recomputed from the raw samples
        per = {v: [statistics.median([min(groups[(v, l, rd)]) for rd in range(1, 5)]) for l in range(3)] for v in 'ab'}
        a, b = statistics.median(per['a']), statistics.median(per['b'])
        change = (b - a) / a * 100
        row = next(l for l in r.stdout.splitlines() if l.startswith('| prog |'))
        self.assertIn(f'{change:+.2f} %', row)
        self.assertIn(f'{a * 1e3:.3f} ms', row)
        self.assertIn(f'{b * 1e3:.3f} ms', row)
        self.assertIn('4,096 -> 4,000', row)
        self.assertIn('rule: noise = max(', r.stdout)
        self.assertIn('pinned to CPU', r.stdout)
        self.assertTrue(os.path.exists(os.path.join(self.work, 'results.md')))
        self.assertEqual(len(read_lines(os.path.join(self.work, 'builds.tsv'))), 1 + 3 * 2)

    # A clear loss, through nxaot_host --repeat (fastest of N in the runner) and B's speed from the environment
    def test_loss_host_repeat_and_env(self):
        r = self.run_tool('--a-args=--fake-speed 0.010', '--b-args=--fake-speed 0.010', '--b-env', 'FAKE_SPEED=0.013')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.verdict(r.stdout), 'slower')
        rows = self.samples()
        self.assertEqual(len(rows), 4 * 3 * 2)
        self.assertTrue(all(x['repeat'] == 'fastest of 3' for x in rows))
        self.assertEqual([x['variant'] for x in rows], ['a', 'b'] * 12)
        for x in rows:
            factor = 1 + 0.01 * (len(os.path.basename(x['file'])) % 3)
            self.assertAlmostEqual(x['seconds'], (0.010 if x['variant'] == 'a' else 0.013) * factor, places=6)

    # A difference smaller than the layouts' spread (2 %): within noise
    def test_noise(self):
        r = self.run_tool('--a-args=--fake-speed 0.010000', '--b-args=--fake-speed 0.010050')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.verdict(r.stdout), 'within noise')
        self.assertIn('noise 1.98 %', r.stdout)

    # The same arguments: identical bytes, no runs
    def test_identical(self):
        r = self.run_tool('--a-args=--fake-speed 0.010', '--b-args=--fake-speed 0.010')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('the packs of A and B are identical bytes', r.stdout)
        self.assertIn('verdict prog: identical packs, not run', r.stdout)
        self.assertFalse(os.path.exists(self.log))

    # B prints another guest output: an error naming the program, exit 1
    def test_differing_output(self):
        r = self.run_tool('--b-args=--fake-out other', layouts=1, rounds=2, repeats=1)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('ERROR prog: variant B (layout 0, round 1) printed a different guest output', r.stdout)
        self.assertIn("'guest says other'", r.stdout)

    # A run that exits non-zero: an error naming the program
    def test_failing_run(self):
        r = self.run_tool('--b-args=--fake-out bad', env={'FAKE_EXIT_bad': '4'}, layouts=1, rounds=1, repeats=1)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('ERROR prog: variant B (layout 0, round 1) exited 4', r.stdout)

    # No taskset on PATH: said, and the runs go unpinned
    def test_unpinned(self):
        r = self.run_tool('--b-args=--fake-speed 0.008', env={'PATH': self.dir}, layouts=2, rounds=2, repeats=1)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('runs NOT pinned: taskset not found', r.stdout)

    # --dry-run prints the commands and runs nothing
    def test_dry_run(self):
        r = self.run_tool('--b-env', 'NXAOT_PROFILE_WEIGHTS=0', '--dry-run', '--cpu', '2', '--set', 'prog:int_ops_rounds=200', layouts=2, rounds=1,
                          repeats=5)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('build b: env NXAOT_PROFILE_WEIGHTS=0 ', r.stdout)
        runs = [l for l in r.stdout.splitlines() if l.startswith('run ')]
        self.assertEqual(len(runs), 4)
        self.assertIn('--repeat 5 --set int_ops_rounds=200', runs[0])
        self.assertFalse(os.path.exists(self.work))
        self.assertFalse(os.path.exists(self.log))


if __name__ == '__main__':
    unittest.main()
