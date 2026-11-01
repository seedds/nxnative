#!/usr/bin/env python3
"""The line table of every pack (CTest `lines`; docs/design/aot-recompiler.md section 3.5, include/nxaot/aot_module.h, README.md "Calls and returns"):
every corpus NRO, the synthetic game module and the host-function module's two of dm/tools/tests/aot_synth.py packed with `nxaot build` (the balanced
mode, as build_packs.sh packs them; two NROs also in the max mode, whose shards inline, and with ghccc), then `nxaot lines` on each pack: every entry's
native address must start an x86 instruction inside the body of the function the table files it under, its guest offset must name an A64 load, store or
dc zva of that function's code (discovery's blocks; at tiers H and H+ of a function inlined into it). The entries of a guest access in a body are every
machine instruction LLVM made from the marked access (a lock cmpxchg and its setcc, ...); the accesses none of whose entries loads or stores (LLVM moved
the mark off a load it folded into its user and unfolded again) are counted and listed, not failed. ABI 2's table was read
as {native, offset} pairs where LLVM's !pcsections writes the addresses of a group of instructions and then their offset once: 246 of fnptr-gcc's 415
entries and 8,812 of switch-gcc's 24,293 were misread.

  test_lines.py --nxaot <nxaot> --work <dir>

Exit 0 when every pack's table holds.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
TOOLS_TESTS = os.path.join(HERE, '..', '..', 'tests')
MORE_MODES = {'fnptr-gcc': [['--mode', 'max'], ['--cc', 'ghc']], 'switch-gcc': [['--mode', 'max']]}


def check(nxaot, work, name, module, extra):
    tag = name + ''.join('-' + a.lstrip('-') for a in extra)
    lib = os.path.join(work, tag + '.so')
    b = subprocess.run([nxaot, 'build', module, '--shared', lib, *extra], capture_output=True, text=True)
    if b.returncode != 0:
        print(f'{tag:<28} FAIL: nxaot build exited {b.returncode}: {(b.stdout + b.stderr)[-2000:]}')
        return False
    m = re.search(r'aot: line table (\d+) entries', b.stdout)
    built = int(m.group(1)) if m else -1
    r = subprocess.run([nxaot, 'lines', lib, module], capture_output=True, text=True)
    m = re.search(r'(\d+) entries in (\d+) of (\d+) functions; (\d+) at loads and stores of their own function\'s code, (\d+) of a function inlined into it; '
                  r'(\d+) bad; (\d+) guest accesses, (\d+) with an x86 load or store among their entries', r.stdout)
    ok = r.returncode == 0 and m is not None and int(m.group(1)) == built and int(m.group(6)) == 0 and built > 0
    if m:
        moved = int(m.group(7)) - int(m.group(8))
        print(f'{tag:<28} {m.group(1):>8} {m.group(2) + "/" + m.group(3):>11} {m.group(4):>9} {m.group(5):>8} {m.group(7):>9} {moved:>6}  {"ok" if ok else "FAIL"}')
        MOVED[0] += moved
        MOVED[1] += int(m.group(7))
    else:
        print(f'{tag:<28} FAIL: {r.stdout[-1500:]}{r.stderr[-500:]}')
    for line in r.stdout.splitlines()[:20]:
        if line.startswith('[RED]') or (line.startswith('nxaot lines: note:') and not tag.endswith(('-max', '-ghc'))):
            print('    ' + line)
    if m and int(m.group(1)) != built:
        print(f'    nxaot build wrote {built} entries, the pack holds {m.group(1)}')
    return ok


MOVED = [0, 0]  # guest accesses none of whose entries is an x86 load or store (LLVM moved the mark), and all accesses


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    sys.path.insert(0, TOOLS_TESTS)
    import aot_synth  # noqa: E402  (dm/tools/tests: the synthetic modules, written here as build_packs.sh writes them)
    aot_synth.write(os.path.join(a.work, 'module'))
    aot_synth.write_hostfn(os.path.join(a.work, 'hostfn'))
    inputs = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nro = line.split('\t')[0]
                inputs.append((nro[:-4], os.path.join(CORPUS, 'nro', nro)))
    inputs += [('module', os.path.join(a.work, 'module', 'main.aot.tsv')), ('hostfn-main', os.path.join(a.work, 'hostfn', 'main.aot.tsv')),
               ('hostfn-sub', os.path.join(a.work, 'hostfn', 'sub.nro'))]
    print(f'{"pack":<28} {"entries":>8} {"functions":>11} {"own code":>9} {"inlined":>8} {"accesses":>9} {"moved":>6}  result')
    failures = runs = 0
    for name, path in inputs:
        for extra in [[]] + MORE_MODES.get(name, []):
            runs += 1
            failures += not check(a.nxaot, a.work, name, path, extra)
    print(f'line tables: {runs} packs, {failures} failures; {MOVED[0]} of {MOVED[1]} guest accesses with no x86 load or store among their entries '
          '(LLVM moved the mark off a folded and unfolded load: a fault there is reported at the nearest entry before it, which says so)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
