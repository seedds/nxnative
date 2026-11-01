#!/usr/bin/env python3
"""Tiers by profile (CTest `profile`; docs/design/aot-recompiler.md section 9.3, README.md "Tiers by profile"): a profile written by hand for corpus
NROs (`nxaot build --profile <file>`: one line per sample, `<offset> <count>`, the guest module offset in hexadecimal and its count in decimal;
`#` starts a comment), with samples inside a few functions (several in one function, one at a function's entry, one at its last instruction), a
sample outside every function and a count of 0. In the balanced mode the sampled functions are compiled at tier H and the others at tier C, in
the max mode at H+ and H; the log's tier counts (`aot: tiers C <n> W <n> H <n> H+ <n>`) and the profile line must say so, and each pack, run
natively, must print exactly corpus/expected/<program>.out. A profile with a malformed line stops the build with the line's number.

  test_profile.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir> [--nro <name>]...

Exit 0 when everything holds.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def functions(nxaot, path, tsv):
    """The discovered functions: (entry, size in bytes), in address order"""
    subprocess.run([nxaot, 'discover', path, '--tsv', tsv], capture_output=True, check=True)
    out = []
    for line in open(tsv):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'fn':
            out.append((int(f[1], 16), int(f[2], 16)))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--nro', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    nros = a.nro or ['fnptr-gcc.nro', 'fp-clang.nro', 'except-gcc.nro', 'switch-gcc.nro']
    programs = {}
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                cols = line.split('\t')
                programs[cols[0]] = cols[1]
    bad = 0
    for nro in nros:
        path = os.path.join(CORPUS, 'nro', nro)
        expected = open(os.path.join(CORPUS, 'expected', programs[nro] + '.out')).read()
        fns = functions(a.nxaot, path, os.path.join(a.work, nro[:-4] + '.discover.tsv'))
        hot = [fns[1], fns[len(fns) // 2], fns[-2]]  # three functions spread over the module
        lines = ['# a profile written by hand: <guest offset, hex> <samples>', '']
        samples = 0
        for k, (entry, size) in enumerate(hot):
            lines.append(f'{entry:#x} {5 + k}')                    # at the entry
            lines.append(f'{entry + size - 4:x}\t{2}')            # at the last instruction, tab-separated, no 0x
            samples += 7 + k
        lines.append(f'{hot[0][0] + 4:#x} 1   # a second offset of the first')
        samples += 1
        outside = max(e + s for e, s in fns) + 0x100000
        lines.append(f'{outside:#x} 3')                           # in no function
        samples += 3
        lines.append(f'{fns[0][0]:#x} 0')                         # sampled zero times: not hot
        offsets = len({int(l.split()[0], 16) for l in lines if l.strip() and not l.startswith('#')})
        profile = os.path.join(a.work, nro[:-4] + '.profile')
        with open(profile, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        n_hot = len({e for e, _ in hot})
        for mode, want in (('balanced', (len(fns) - n_hot, 0, n_hot, 0)), ('max', (0, 0, len(fns) - n_hot, n_hot))):
            so = os.path.join(a.work, f'{nro[:-4]}.{mode}.so')
            b = run([a.nxaot, 'build', path, '--shared', so, '--mode', mode, '--profile', profile])
            t = re.search(r'^aot: tiers C (\d+) W (\d+) H (\d+) H\+ (\d+)', b.stdout, re.M)
            p = re.search(r'^aot: profile \S+: (\d+) samples at (\d+) offsets, (\d+) outside every function', b.stdout, re.M)
            got = tuple(int(x) for x in t.groups()) if t else None
            why = ''
            if b.returncode != 0 or not t or not p:
                why = f'nxaot build exit {b.returncode}: {(b.stdout + b.stderr).strip().splitlines()[-1:]}'
            elif got != want:
                why = f'tiers C W H H+ {got}, want {want}'
            elif (int(p.group(1)), int(p.group(2)), int(p.group(3))) != (samples, offsets, 1):
                why = f'profile line {p.group(0)!r}, want {samples} samples at {offsets} offsets, 1 outside'
            else:
                r = run([a.nxaot_host, so, path])
                if r.returncode != 0 or r.stdout != expected:
                    why = f'native run exit {r.returncode}, output {"differs" if r.stdout != expected else "matches"}'
            bad += bool(why)
            print(f'{nro:<16} {mode:<9} tiers C W H H+ {got}  {why or "ok"}')
    # A malformed line stops the build, naming it
    broken = os.path.join(a.work, 'broken.profile')
    with open(broken, 'w') as f:
        f.write('0x100 3\nnot-a-number 2\n')
    b = run([a.nxaot, 'build', os.path.join(CORPUS, 'nro', nros[0]), '--shared', os.path.join(a.work, 'broken.so'), '--profile', broken])
    ok = b.returncode != 0 and 'broken.profile:2:' in b.stdout
    bad += not ok
    print(f'a malformed profile: exit {b.returncode}, {"names line 2" if ok else "FAIL"}')
    print(f'tiers by profile: {len(nros)} NROs x 2 modes and a malformed profile, {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
