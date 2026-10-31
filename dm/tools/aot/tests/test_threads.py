#!/usr/bin/env python3
"""Guest threads on the corpus (CTest `corpus_threads`; docs/design/aot-recompiler.md sections 6.1, 6.3 and 6.5): the programs that make threads
(corpus.tsv rows whose expected output has `threads.` lines: excl, whose workers increment shared counters with ldaxr / stlxr loops, a
compare-and-swap loop, ldaxp / stlxp pairs and a mutex on ArbitrateLock / ArbitrateUnlock, with exact totals checked by the program itself)
built by `nxaot build` and run many times natively, with real host threads contending (each run must print exactly
corpus/expected/<program>.out), and a few times with everything in the JIT (one dynarmic instance per thread, the shared exclusive monitor).

  test_threads.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir> [--runs N] [--jit-runs N] [--nro <name>]...

Prints per NRO and mode the runs, the failures, and the fastest and slowest run. Exit 0 when every run matched.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


def threaded_rows():
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            nro, program = line.rstrip('\n').split('\t')[:2]
            expected = open(os.path.join(CORPUS, 'expected', program + '.out')).read()
            if '\nthreads.' in expected:
                rows.append((nro, program, expected))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--runs', type=int, default=25)
    ap.add_argument('--jit-runs', type=int, default=5)
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows = [r for r in threaded_rows() if not a.nro or r[0] in a.nro]
    bad = 0
    total = 0
    print(f'{"nro":<16} {"mode":<8} {"runs":>5} {"failed":>6} {"fastest ms":>10} {"slowest ms":>10} {"threads":>7}')
    for nro, program, expected in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        so = os.path.join(a.work, nro[:-4] + '.so')
        b = subprocess.run([a.nxaot, 'build', path, '-o', so[:-3] + '.o', '--shared', so], capture_output=True, text=True)
        if b.returncode != 0:
            bad += 1
            print(f'{nro:<16} FAIL: nxaot build exit {b.returncode}')
            continue
        for mode, flags, n in (('native', [], a.runs), ('jit-all', ['--jit-all'], a.jit_runs)):
            failed = 0
            times = []
            threads = 0
            for i in range(n):
                r = subprocess.run([a.nxaot_host, so, path] + flags, capture_output=True, text=True)
                m = re.search(r': \S+ after ([0-9.]+) s .* (\d+) threads;', r.stderr)
                if m:
                    times.append(float(m.group(1)) * 1e3)
                    threads = int(m.group(2))
                if r.returncode != 0 or r.stdout != expected:
                    failed += 1
                    if failed <= 3:
                        print(f'    {nro} {mode} run {i}: exit {r.returncode}')
                        for j, (g, w) in enumerate(zip(r.stdout.splitlines(), expected.splitlines())):
                            if g != w:
                                print(f'      line {j + 1}: got {g!r}, want {w!r}')
                                break
                        for line in (r.stdout + r.stderr).splitlines():
                            if line.startswith('[RED]'):
                                print('      ' + line)
            total += n
            bad += failed
            print(f'{nro:<16} {mode:<8} {n:>5} {failed:>6} {min(times or [0]):>10.3f} {max(times or [0]):>10.3f} {threads:>7}')
    print(f'corpus threads: {len(rows)} NROs, {total} runs (exact totals each), {bad} failures')
    return 1 if bad or not rows else 0


if __name__ == '__main__':
    sys.exit(main())
