#!/usr/bin/env python3
"""The corpus recompiled and run natively (CTest `corpus_native`; docs/design/aot-recompiler.md section 14, phase 1): every NRO of corpus.tsv is
built by `nxaot build` into a shared object in each configuration below, run by nxaot_host, and must leave through svcExitProcess printing
exactly corpus/expected/<program>.out and exactly what the dynarmic reference (nxaot_run) prints. Then two checks of the dispatcher (section 4):
the lookup table of every NRO holds every function discovery found address-taken, and an indirect branch to a target with no compiled entry (a
function pointer of fnptr set to the middle of a function) stops with the [RED] line naming it and the JIT-needed exit status.

  test_corpus_native.py --nxaot <nxaot> --nxaot-host <nxaot_host> --nxaot-run <nxaot_run> --work <dir> [--configs a,b,...] [--nro <name>]...

Configurations: swifttail and ghc in the balanced mode (tier W), the default convention (swifttail) in the correctness mode (every register through
the state block at every call), and the default convention in the max mode (tier H+). Prints one line per NRO, convention and mode (the result, the
lines, the guest run time natively and under dynarmic; the dynarmic time includes its JIT's compilation, the native one does not: that is the pack's)
and the totals. Exit 0 when everything matched.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
JIT_NEEDED = 3  # rt::kJit

# name: (convention, mode, nxaot build flags); the default convention is swifttail
CONFIGS = {
    'swifttail': ('swifttail', 'balanced', ['--cc', 'swifttail']),
    'ghc': ('ghc', 'balanced', ['--cc', 'ghc']),
    'correctness': ('swifttail', 'correctness', ['--correctness']),
    'max': ('swifttail', 'max', ['--mode', 'max']),
}


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


def seconds(stderr, pattern):
    m = re.search(pattern, stderr)
    return float(m.group(1)) if m else float('nan')


def build(a, nro, name, flags):
    so = os.path.join(a.work, name + '.so')
    obj = os.path.join(a.work, name + '.o')
    b = run([a.nxaot, 'build', os.path.join(CORPUS, 'nro', nro), '-o', obj, '--shared', so] + flags)
    return so, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--nxaot-run', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--configs', default='swifttail,ghc,correctness,max')
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    configs = a.configs.split(',')
    rows = [r for r in corpus_rows() if not a.nro or r[0] in a.nro]
    bad = 0
    print(f'{"nro":<20} {"convention":<11} {"mode":<12} {"result":<8} {"lines":>5} {"native ms":>10} {"dynarmic ms":>12}')
    for nro, program, toolchain in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        expected = open(os.path.join(CORPUS, 'expected', program + '.out')).read()
        ref = run([a.nxaot_run, path])
        ref_ms = seconds(ref.stderr, r'in ([0-9.]+) s') * 1e3
        if ref.returncode != 0 or ref.stdout != expected:
            print(f'{nro:<20} {"dynarmic":<11} {"-":<12} FAIL     reference run differs from expected/{program}.out (exit {ref.returncode})')
            bad += 1
        for config in configs:
            cc, mode, flags = CONFIGS[config]
            name = f'{nro[:-4]}.{config}'
            so, b = build(a, nro, name, flags)
            if b.returncode != 0:
                bad += 1
                print(f'{nro:<20} {cc:<11} {mode:<12} FAIL     nxaot build exit {b.returncode}: {(b.stdout + b.stderr).strip().splitlines()[-1:]}')
                continue
            r = run([a.nxaot_host, so, path])
            ms = seconds(r.stderr, r'after ([0-9.]+) s') * 1e3
            why = ''
            if r.returncode != 0:
                why = f'exit {r.returncode}: ' + (r.stdout.strip().splitlines() or ['-'])[-1]
            elif r.stdout != expected:
                why = f'output differs from expected/{program}.out'
            elif r.stdout != ref.stdout:
                why = 'output differs from the dynarmic run'
            if why:
                bad += 1
                print(f'{nro:<20} {cc:<11} {mode:<12} FAIL     {why}')
                for i, (g, w) in enumerate(zip(r.stdout.splitlines(), expected.splitlines())):
                    if g != w:
                        print(f'    line {i + 1}: got {g!r}, want {w!r}')
                        break
            else:
                print(f'{nro:<20} {cc:<11} {mode:<12} {"ok":<8} {len(r.stdout.splitlines()):>5} {ms:>10.3f} {ref_ms:>12.3f}')

        # The lookup table (section 4) lists every address-taken function of discovery
        tsv = os.path.join(a.work, nro[:-4] + '.discover.tsv')
        d = run([a.nxaot, 'discover', path, '--tsv', tsv])
        taken = set()
        if d.returncode == 0:
            for line in open(tsv):
                f = line.rstrip('\n').split('\t')
                if f[0] == 'fn' and f[15] == '1':
                    taken.add(int(f[1], 16))
        so = os.path.join(a.work, f'{nro[:-4]}.{configs[0]}.so')
        lk = run([a.nxaot_host, so, path, '--lookup'])
        listed = {int(x, 16) for x in lk.stdout.split()} if lk.returncode == 0 else set()
        missing = sorted(taken - listed)
        if d.returncode != 0 or lk.returncode != 0 or missing or not taken:
            bad += 1
            print(f'{nro:<20} {"lookup":<24} FAIL     {len(taken)} address-taken, {len(listed)} listed, missing {[hex(m) for m in missing][:8]}')
        else:
            print(f'{nro:<20} {"lookup":<24} {"ok":<8} {len(taken)} address-taken functions, all in the lookup table ({len(listed)} entries)')

        # A deliberately unknown target: fnptr's hidden_ptr (called through blr) set to the second instruction of hidden_target
        if program == 'fnptr':
            syms = {}
            for line in open(os.path.join(CORPUS, 'nro', nro[:-4] + '.syms.tsv')):
                f = line.rstrip('\n').split('\t')
                if len(f) >= 5 and not line.startswith("#"):
                    syms[f[4]] = int(f[0], 16)
            target = syms['hidden_target'] + 4
            r = run([a.nxaot_host, so, path, '--relocate', '--set', f'hidden_ptr=@{target:#x}'])
            want = f'(module+{target:#x}) has no compiled entry'
            ok = r.returncode == JIT_NEEDED and any(l.startswith('[RED] aot: ') and want in l for l in r.stdout.splitlines())
            if not ok:
                bad += 1
                print(f'{nro:<20} {"miss":<24} FAIL     exit {r.returncode}, last line {(r.stdout.strip().splitlines() or ["-"])[-1]!r}')
            else:
                red = [l for l in r.stdout.splitlines() if l.startswith('[RED] aot: ')][0]
                print(f'{nro:<20} {"miss":<24} {"ok":<8} exit {r.returncode}: {red}')
    total = len(rows) * len(configs)
    print(f'corpus native run: {len(rows)} NROs x {len(configs)} configurations ({total} runs) and the dispatcher checks, {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
