#!/usr/bin/env python3
"""The pack stage's convention facts after an indirect call (CTest `plan`; docs/design/aot-recompiler.md section 2.3, README.md "Calls and
returns"): an indirect call (blr, a br discovery could not bound) may reach a function outside the module (a host function through a GOT slot,
another pack, code the JIT fallback runs), which leaves its results in AAPCS64's result registers, x0..x8 and v0..v3. For every corpus NRO, the
synthetic game module and the host-function module of dm/tools/tests/aot_synth.py, `nxaot plan` must show those registers in the indirect view's
WRITES (what a call site reloads after an indirect call) and in the WRITES of every function with an indirect site (what its callers reload).

The host-function module is also checked with its one function that writes v0 (hf_one, `fmov s0, #1.0`) made a nop: then no instruction of the
module writes v0, the case where the reload set used to be the module's own union and a call site kept its own v0 after sqrtf returned a float
through the GOT (README.md, "Pack throughput").

Every module's standard entries must have the one signature of ABI 3 (include/nxaot/aot_module.h), whatever its address-taken functions read or
write: x0..x7 and v0..v7 in host registers, rv 0, so that a call site of one pack may enter another pack's standard entry directly.

  test_plan.py --nxaot <nxaot> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
TOOLS_TESTS = os.path.join(HERE, '..', '..', 'tests')
FOREIGN = {f'x{i}' for i in range(9)} | {f'v{i}' for i in range(4)}  # AAPCS64's result registers (analysis::ForeignWrites)
STD_PASS = {f'x{i}' for i in range(8)} | {f'v{i}' for i in range(8)}  # the standard entries' fixed signature under swifttailcc (build/plan.h StdPass)
NOP = 0xD503201F


def regs(text):
    """'x0-x3,x30,sp,v0,nzcv' -> {'x0', 'x1', 'x2', 'x3', 'x30', 'sp', 'v0', 'nzcv'}"""
    out = set()
    if text == '-':
        return out
    for part in text.split(','):
        if '-' in part:
            lo, hi = part.split('-')
            out |= {f'{lo[0]}{i}' for i in range(int(lo[1:]), int(hi[1:]) + 1)}
        else:
            out.add(part)
    return out


def plan(nxaot, path):
    r = subprocess.run([nxaot, 'plan', path], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'test_plan.py: nxaot plan {path} failed (exit {r.returncode}): {r.stdout}{r.stderr}')
    indirect, fns = None, []
    for line in r.stdout.splitlines():
        f = line.split('\t')
        if f[0] == 'indirect':
            indirect = dict(zip(f[1::2], f[2::2]))
        elif f[0] == 'fn':
            fns.append((f[1], dict(zip(f[2::2], f[3::2]))))
    return indirect, fns


def check(nxaot, name, path):
    indirect, fns = plan(nxaot, path)
    bad = []
    missing = FOREIGN - regs(indirect['writes'])
    if missing:
        bad.append(f'the indirect view does not reload {sorted(missing)}')
    # ABI 3: the standard entries' one signature, the same in every pack (a call may enter another pack's standard entry directly): under swifttailcc
    # x0..x7 and v0..v7 in host registers, x0 and x1 returned in host registers and v0..v3 through the block
    if regs(indirect['pass']) != STD_PASS or indirect['rv'] != '0':
        bad.append(f'the standard entries\' signature is pass {indirect["pass"]} rv {indirect["rv"]}, not the fixed x0-x7,v0-v7 rv 0')
    sites = [(e, f) for e, f in fns if int(f['indirect'])]
    for e, f in sites:
        m = FOREIGN - regs(f['writes'])
        if m:
            bad.append(f'the function at {e} has an indirect site and its WRITES lack {sorted(m)}')
    print(f'{name:<24} {len(fns):>9} {len(sites):>14}  {indirect["writes"]:<28} {"ok" if not bad else "FAIL"}')
    for b in bad[:5]:
        print('    ' + b)
    return not bad


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
    aot_synth.FMOV_S0_1 = NOP  # hf_one no longer writes v0: no function of the module does
    aot_synth.write_hostfn(os.path.join(a.work, 'hostfn-no-v0'))
    inputs = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nro = line.split('\t')[0]
                inputs.append((nro[:-4], os.path.join(CORPUS, 'nro', nro)))
    inputs += [('module', os.path.join(a.work, 'module', 'main.aot.tsv')), ('hostfn main', os.path.join(a.work, 'hostfn', 'main.aot.tsv')),
               ('hostfn sub', os.path.join(a.work, 'hostfn', 'sub.nro')), ('hostfn main, no v0', os.path.join(a.work, 'hostfn-no-v0', 'main.aot.tsv'))]
    print(f'{"module":<24} {"functions":>9} {"indirect sites":>14}  {"indirect view reloads":<28} result')
    failures = 0
    for name, path in inputs:
        failures += not check(a.nxaot, name, path)
    print(f'indirect calls reload AAPCS64\'s result registers: {len(inputs)} modules, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
