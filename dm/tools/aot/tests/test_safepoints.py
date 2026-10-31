#!/usr/bin/env python3
"""Safepoints (CTest `safepoints`; docs/design/aot-recompiler.md section 6.2), in the manner of dm/host/tests/t_preempt.c: nxaot_host
--pause-test N runs a host thread beside the guest's that stops every guest thread N times at random intervals while more than one of them
runs (the pause protocol of dm/host/hle_thread.c: a thread in compiled code stops at its next poll, at a function entry or a backward branch,
with its whole register file in its state block; one in an SVC at once; one in the JIT at its next block boundary), reads every state block
while the world is stopped, checks it (pc a guest instruction, sp in the arena, and at the head of corpus/rt/pause.S's loop the register
invariant x20 = 2 * x19, x21 = ~x19, which a block written back incompletely or from another moment breaks), and resumes them. The threaded
corpus programs (excl, with its register-invariant thread made ten times longer: --set excl_pause_iterations, so that the pauses fall while
several threads run) run so with several seeds natively and with everything in the JIT; each run must still print exactly
corpus/expected/<program>.out, make its N pauses with more than one thread running, and find no inconsistent block; over the runs the threads
must have stopped at polls (natively) and in the JIT, and the invariant must have been checked.

  test_safepoints.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir> [--pauses N] [--seeds N] [--jit-seeds N] [--nro <name>]...

Exit 0 when every run passed.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
PAUSE = re.compile(r'pause test: (\d+) pauses \((\d+) with more than one thread running\), thread stops: (\d+) at polls, (\d+) in SVCs, (\d+) in the JIT; '
                   r'(\d+) register-invariant checks, (\d+) inconsistent; longest stop ([0-9.]+) us')


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
    ap.add_argument('--pauses', type=int, default=50)
    ap.add_argument('--seeds', type=int, default=6)
    ap.add_argument('--jit-seeds', type=int, default=2)
    ap.add_argument('--pause-iterations', default='200000000')
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows = [r for r in threaded_rows() if not a.nro or r[0] in a.nro]
    bad = 0
    print(f'{"nro":<16} {"mode":<8} {"seed":>4} {"result":<6} {"pauses":>6} {"threaded":>8} {"polls":>5} {"svcs":>5} {"jit":>5} {"invariant":>9} '
          f'{"bad":>4} {"longest us":>10}')
    for nro, program, expected in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        so = os.path.join(a.work, nro[:-4] + '.so')
        b = subprocess.run([a.nxaot, 'build', path, '-o', so[:-3] + '.o', '--shared', so], capture_output=True, text=True)
        if b.returncode != 0:
            bad += 1
            print(f'{nro:<16} FAIL: nxaot build exit {b.returncode}')
            continue
        for mode, flags, seeds in (('native', [], a.seeds), ('jit-all', ['--jit-all'], a.jit_seeds)):
            sums = [0] * 7
            for seed in range(1, seeds + 1):
                r = subprocess.run([a.nxaot_host, so, path, '--pause-test', str(a.pauses), '--pause-seed', str(seed),
                                    '--set', f'excl_pause_iterations={a.pause_iterations}'] + flags, capture_output=True, text=True)
                m = PAUSE.search(r.stderr)
                ok = r.returncode == 0 and r.stdout == expected and m is not None
                if m:
                    v = [int(x) for x in m.groups()[:7]]
                    ok = ok and v[1] == a.pauses and v[6] == 0
                    sums = [s + x for s, x in zip(sums, v)]
                if not ok:
                    bad += 1
                    print(f'{nro:<16} {mode:<8} {seed:>4} FAIL   exit {r.returncode}, output {"same" if r.stdout == expected else "differs"}')
                    for line in (r.stdout + r.stderr).splitlines():
                        if line.startswith('[RED]'):
                            print('    ' + line)
                    continue
                print(f'{nro:<16} {mode:<8} {seed:>4} {"ok":<6} {m.group(1):>6} {m.group(2):>8} {m.group(3):>5} {m.group(4):>5} {m.group(5):>5} '
                      f'{m.group(6):>9} {m.group(7):>4} {float(m.group(8)):>10.1f}')
            # Over the seeds: the threads stopped where this mode's code stops (polls natively, block boundaries in the JIT), and the
            # register invariant was read at the loop head
            where = sums[2] if mode == 'native' else sums[4]
            if seeds and (where == 0 or sums[5] == 0):
                bad += 1
                print(f'{nro:<16} {mode:<8} FAIL   over {seeds} seeds: {where} stops {"at polls" if mode == "native" else "in the JIT"}, '
                      f'{sums[5]} invariant checks (want both above 0)')
    print(f'safepoints: {len(rows)} NROs, pause tests of {a.pauses} pauses, {a.seeds} seeds natively and {a.jit_seeds} in the JIT, {bad} failures')
    return 1 if bad or not rows else 0


if __name__ == '__main__':
    sys.exit(main())
