#!/usr/bin/env python3
"""The differential test per function on the corpus (CTest `corpus_funcdiff`; docs/design/aot-recompiler.md section 12, item 2): every NRO of
corpus.tsv built with --all-entries (every function gets a standard entry) in the correctness mode, where every register is compared, and in the
summary-driven mode, where what the convention hands a caller is compared; nxaot_funcdiff runs every function from the same register and memory
snapshot under the recompiled code and under dynarmic.

  test_funcdiff.py --nxaot <nxaot> --nxaot-funcdiff <nxaot_funcdiff> --work <dir> [--seeds N] [--cc swifttail|ghc] [--nro <name>]...

Exit 0 when no run differs.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-funcdiff', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--seeds', default='3')
    ap.add_argument('--cc', default='swifttail')
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    nros = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nros.append(line.split('\t')[0])
    if a.nro:
        nros = [n for n in nros if n in a.nro]
    bad = 0
    total_compared = 0
    print(f'{"nro":<20} {"mode":<12} {"functions":>9} {"compared":>9} {"mismatches":>10}  other outcomes')
    for nro in nros:
        path = os.path.join(CORPUS, 'nro', nro)
        for mode, flags in (('correctness', ['--correctness']), ('summary', [])):
            so = os.path.join(a.work, f'{nro[:-4]}.{mode}.all.so')
            b = subprocess.run([a.nxaot, 'build', path, '-o', so[:-3] + '.o', '--shared', so, '--all-entries', '--cc', a.cc] + flags,
                               capture_output=True, text=True)
            if b.returncode != 0:
                bad += 1
                print(f'{nro:<20} {mode:<12} FAIL: nxaot build exit {b.returncode}')
                continue
            r = subprocess.run([a.nxaot_funcdiff, so, path, '--seeds', a.seeds], capture_output=True, text=True)
            m = re.search(r'== nxaot_funcdiff \S+: (\d+) functions x \d+ seeds, (\d+) compared .*, (\d+) mismatches ==', r.stdout)
            other = ', '.join(f'{k.strip()} {n}' for k, n in re.findall(r'^  (\S.*?)\s+(\d+)$', r.stdout, re.M) if not k.startswith('compared'))
            if not m or r.returncode != 0:
                bad += 1
                print(f'{nro:<20} {mode:<12} FAIL (exit {r.returncode})')
                for line in r.stdout.splitlines():
                    if line.startswith('MISMATCH') or line.startswith('[RED]'):
                        print('    ' + line)
                continue
            total_compared += int(m.group(2))
            print(f'{nro:<20} {mode:<12} {m.group(1):>9} {m.group(2):>9} {m.group(3):>10}  {other}')
    print(f'corpus per-function differential test ({a.cc}): {len(nros)} NROs x 2 modes, {total_compared} runs compared, {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
