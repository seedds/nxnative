#!/usr/bin/env python3
"""The JIT fallback on the corpus (CTest `corpus_jit`; docs/design/aot-recompiler.md sections 11 and 14, phase 3): every NRO of corpus.tsv built
by `nxaot build` (the default: swifttailcc, balanced) and run by nxaot_host three ways, which must print the same lines, exactly
corpus/expected/<program>.out:
  mixed      compiled entries natively, the JIT for the rest (the default); its JIT entries are counted (the fiber program's continuations
             with no native frame, design 3.4)
  jit-all    everything in the JIT: no compiled entry is taken
  jit-none   no JIT: an NRO whose mixed run needed none must print the same; one that did must stop with status 3 and the [RED] line
Then the dispatcher's miss path (section 4, section 11 case 1): fnptr's hidden_ptr, called through blr, set to the second instruction of
hidden_target (an address with no compiled entry): the mixed run must enter the JIT there (logged once as a [RED] line naming it), come back to
compiled code, and print what the jit-all run prints; the jit-none run must stop with status 3.

  test_corpus_jit.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir> [--nro <name>]...

Prints a table per NRO (result, the guest time natively and in the JIT, the JIT's runs, their start addresses, Run() calls, the stops it stepped
over and the native calls it made) and the totals. Exit 0 when everything matched.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
JIT_NEEDED = 3  # rt::kJit


def corpus_rows():
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            nro, program, toolchain = line.rstrip('\n').split('\t')[:3]
            rows.append((nro, program, toolchain))
    return rows


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def stats(stderr):
    """The summary lines of nxaot_host: seconds, and the JIT's counts"""
    out = {'ms': float('nan'), 'runs': 0, 'addresses': 0, 'run_calls': 0, 'steps': 0, 'native': 0, 'jit_ms': 0.0}
    m = re.search(r': \S+ after ([0-9.]+) s', stderr)
    if m:
        out['ms'] = float(m.group(1)) * 1e3
    m = re.search(r'jit \([a-z-]+\): (\d+) runs from (\d+) addresses, (\d+) Run\(\) calls, (\d+) stepped stops, (\d+) native calls, ([0-9.]+) s', stderr)
    if m:
        out.update(runs=int(m.group(1)), addresses=int(m.group(2)), run_calls=int(m.group(3)), steps=int(m.group(4)), native=int(m.group(5)),
                   jit_ms=float(m.group(6)) * 1e3)
    return out


def symbols(nro):
    syms = {}
    for line in open(os.path.join(CORPUS, 'nro', nro[:-4] + '.syms.tsv')):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 5 and not line.startswith('#'):
            syms[f[4]] = int(f[0], 16)
    return syms


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
    print(f'{"nro":<18} {"mixed":<6} {"native ms":>9} {"jit runs":>8} {"addrs":>5} {"natcalls":>8} {"jit-all":<7} {"jit ms":>8} {"Run()":>6} '
          f'{"jit-none":<16}')
    for nro, program, toolchain in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        expected = open(os.path.join(CORPUS, 'expected', program + '.out')).read()
        so = os.path.join(a.work, nro[:-4] + '.so')
        b = run([a.nxaot, 'build', path, '-o', so[:-3] + '.o', '--shared', so])
        if b.returncode != 0:
            bad += 1
            print(f'{nro:<18} FAIL: nxaot build exit {b.returncode}: {(b.stdout + b.stderr).strip().splitlines()[-1:]}')
            continue
        results = {}
        for mode, flags in (('mixed', []), ('jit-all', ['--jit-all']), ('jit-none', ['--jit-none'])):
            r = run([a.nxaot_host, so, path] + flags)
            results[mode] = (r, stats(r.stderr))
        mixed, ms = results['mixed']
        alljit, js = results['jit-all']
        none, ns = results['jit-none']
        why = []
        if mixed.returncode != 0 or mixed.stdout != expected:
            why.append(f'mixed: exit {mixed.returncode}' + ('' if mixed.returncode else ', output differs from expected'))
        if alljit.returncode != 0 or alljit.stdout != expected:
            why.append(f'jit-all: exit {alljit.returncode}' + ('' if alljit.returncode else ', output differs from expected'))
        if js['runs'] == 0 or js['native']:
            why.append(f'jit-all: {js["runs"]} JIT runs, {js["native"]} native calls (want at least one run and no native call)')
        if ms['runs'] == 0:
            none_ok = none.returncode == 0 and none.stdout == expected
            none_text = 'same' if none_ok else f'FAIL exit {none.returncode}'
        else:
            red = [l for l in none.stdout.splitlines() if l.startswith('[RED] aot: ') and '--jit-none' in l]
            none_ok = none.returncode == JIT_NEEDED and bool(red)
            none_text = 'refused (exit 3)' if none_ok else f'FAIL exit {none.returncode}'
        if not none_ok:
            why.append('jit-none: ' + none_text)
        if why:
            bad += 1
            print(f'{nro:<18} FAIL     {"; ".join(why)}')
            continue
        print(f'{nro:<18} {"ok":<6} {ms["ms"]:>9.3f} {ms["runs"]:>8} {ms["addresses"]:>5} {ms["native"]:>8} {"ok":<7} {js["ms"]:>8.3f} '
              f'{js["run_calls"]:>6} {none_text:<16}')

        # The dispatcher's miss path into the JIT: fnptr's hidden_ptr set to the second instruction of hidden_target
        if program == 'fnptr':
            target = symbols(nro)['hidden_target'] + 4
            sets = ['--relocate', '--set', f'hidden_ptr=@{target:#x}']
            mixed = run([a.nxaot_host, so, path] + sets)
            alljit = run([a.nxaot_host, so, path, '--jit-all'] + sets)
            none = run([a.nxaot_host, so, path, '--jit-none'] + sets)
            s = stats(mixed.stderr)
            want = f'(module+{target:#x}) runs in the JIT fallback'
            logged = [l for l in mixed.stderr.splitlines() if l.startswith('[RED] aot: ') and want in l]
            refused = [l for l in none.stdout.splitlines() if l.startswith('[RED] aot: ') and f'(module+{target:#x}) has no compiled entry' in l]
            ok = (mixed.returncode == 0 and alljit.returncode == 0 and mixed.stdout == alljit.stdout and s['runs'] >= 1 and len(logged) == 1 and
                  none.returncode == JIT_NEEDED and refused)
            if not ok:
                bad += 1
                print(f'{nro:<18} {"miss":<6} FAIL     mixed exit {mixed.returncode}, jit-all exit {alljit.returncode}, same output '
                      f'{mixed.stdout == alljit.stdout}, {s["runs"]} JIT runs, logged {len(logged)}, jit-none exit {none.returncode}')
            else:
                print(f'{nro:<18} {"miss":<6} ok: {s["runs"]} JIT run at module+{target:#x}, back to compiled code, the jit-all output; '
                      f'{logged[0]}; jit-none: {refused[0]}')
    print(f'corpus JIT run: {len(rows)} NROs x 3 modes (mixed, jit-all, jit-none) and the dispatcher miss, {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
