#!/usr/bin/env python3
"""Functions split for size (CTest `split`; docs/design/aot-recompiler.md section 9.3, README.md "Functions split for size"): every NRO of
corpus.tsv packed with the split limit lowered to 64 instructions (--split-above 64: parts of 16 to 32 instructions), so that nearly every function
of the corpus runs as parts joined by tail calls through the state block, across calls, loops, jump tables, landing pads (except), setjmp
continuations (sjlj), fibers and threads; in both conventions and the correctness mode. Each pack must split functions, run natively (nxaot_host)
printing exactly corpus/expected/<program>.out, and pass `nxaot lines` (the line table's entries of the parts, from the joined object, at x86
loads and stores of the function's code). Then switch-gcc at the default limit: sw_word (44,059 instructions) splits and the pack prints the
expected output.

  test_split.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir> [--nro <name>]...

Exit 0 when everything holds.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')

CONFIGS = {
    'swifttail': ['--cc', 'swifttail', '--split-above', '64'],
    'ghc': ['--cc', 'ghc', '--split-above', '64'],
    'correctness': ['--correctness', '--split-above', '64'],
}


def corpus_rows():
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            nro, program = line.rstrip('\n').split('\t')[:2]
            rows.append((nro, program))
    return rows


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows = [r for r in corpus_rows() if not a.nro or r[0] in a.nro]
    bad = 0
    total_split = total_parts = 0
    print(f'{"nro":<20} {"config":<12} {"split":>6} {"parts":>6} {"transfers":>9}  result')
    for nro, program in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        expected = open(os.path.join(CORPUS, 'expected', program + '.out')).read()
        for config, flags in CONFIGS.items():
            so = os.path.join(a.work, f'{nro[:-4]}.{config}.so')
            b = run([a.nxaot, 'build', path, '--shared', so] + flags)
            m = re.search(r'^aot: split functions (\d+) into (\d+) parts, (\d+) transfers', b.stdout, re.M)
            if b.returncode != 0 or not m:
                bad += 1
                print(f'{nro:<20} {config:<12} FAIL nxaot build exit {b.returncode}: {(b.stdout + b.stderr).strip().splitlines()[-1:]}')
                continue
            fns, parts, transfers = (int(x) for x in m.groups())
            total_split += fns
            total_parts += parts
            r = run([a.nxaot_host, so, path])
            why = ''
            if fns == 0:
                why = 'no function split'
            elif r.returncode != 0:
                why = f'exit {r.returncode}: ' + (r.stdout.strip().splitlines() or ['-'])[-1]
            elif r.stdout != expected:
                why = f'output differs from expected/{program}.out'
            if not why and config == 'swifttail':
                lines = run([a.nxaot, 'lines', so, path])
                if lines.returncode != 0:
                    why = 'nxaot lines: ' + (lines.stdout.strip().splitlines() or ['-'])[-1]
            bad += bool(why)
            print(f'{nro:<20} {config:<12} {fns:>6} {parts:>6} {transfers:>9}  {why or "ok"}')

    # The default limit: switch-gcc's sw_word, the corpus's one function over it
    path = os.path.join(CORPUS, 'nro', 'switch-gcc.nro')
    so = os.path.join(a.work, 'switch-gcc.default.so')
    b = run([a.nxaot, 'build', path, '--shared', so, '--workers', '8'])
    m = re.search(r'^aot: split functions (\d+) into (\d+) parts', b.stdout, re.M)
    r = run([a.nxaot_host, so, path])
    expected = open(os.path.join(CORPUS, 'expected', 'switch.out')).read()
    ok = b.returncode == 0 and m and int(m.group(1)) == 1 and r.returncode == 0 and r.stdout == expected
    bad += not ok
    print(f'switch-gcc at the default limit: {m.group(0) if m else "no split line"}; {"ok" if ok else "FAIL"}')
    print(f'functions split for size: {len(rows)} NROs x {len(CONFIGS)} configurations, {total_split} functions in {total_parts} parts; '
          f'{bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
