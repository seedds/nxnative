#!/usr/bin/env python3
"""The corpus's reference run (CTest `corpus_ref`): every NRO of corpus.tsv under dynarmic's A64 JIT (nxaot_run) must leave through
svcExitProcess and print exactly corpus/expected/<program>.out, the lines a native run on ARM64 hardware and the recompiled code must print too.

  test_corpus_run.py --nxaot-run <path to nxaot_run> [--write]

--write records the outputs of the GCC builds as the expected files (after a change to a corpus program; review the diff). Exit 0 when every NRO
matches.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


def corpus_rows():
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            nro, program, toolchain = line.rstrip('\n').split('\t')[:3]
            rows.append((nro, program, toolchain))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot-run', required=True)
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    bad = 0
    rows = corpus_rows()
    print(f'{"nro":<22} {"exit":>4} {"lines":>5}  result')
    for nro, program, toolchain in rows:
        run = subprocess.run([a.nxaot_run, os.path.join(CORPUS, 'nro', nro)], capture_output=True, text=True)
        expected_path = os.path.join(CORPUS, 'expected', program + '.out')
        if a.write and toolchain == 'gcc':
            with open(expected_path, 'w', newline='\n') as f:
                f.write(run.stdout)
        expected = open(expected_path).read() if os.path.exists(expected_path) else None
        last = run.stdout.strip().splitlines()[-1] if run.stdout.strip() else '-'
        if run.returncode != 0 or run.stdout != expected:
            bad += 1
            why = 'exit status' if run.returncode else 'no expected file' if expected is None else f'output differs from expected/{program}.out'
            print(f'{nro:<22} {run.returncode:>4} {len(run.stdout.splitlines()):>5}  FAIL ({why}); last line: {last}')
            if expected is not None:
                for i, (g, w) in enumerate(zip(run.stdout.splitlines(), expected.splitlines())):
                    if g != w:
                        print(f'    line {i + 1}: got {g!r}, want {w!r}')
                        break
        else:
            print(f'{nro:<22} {run.returncode:>4} {len(run.stdout.splitlines()):>5}  ok ({last})')
    print(f'corpus reference run: {len(rows)} NROs, {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
