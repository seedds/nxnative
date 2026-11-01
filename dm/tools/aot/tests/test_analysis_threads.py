#!/usr/bin/env python3
"""The analysis on threads gives the analysis on one thread (CTest `analysis_threads`; README.md, "Phase 5c"): discovery's walks, jump tables,
summaries, windows and scans and the plan's fixed points run on a pool of threads, and every result must be byte for byte the one-thread
result. For every corpus NRO, the synthetic game module and the host-function module's two of dm/tools/tests/aot_synth.py (and any --module
given, such as the game-shaped benchmarks of gen_bench.py):

- `nxaot discover --tsv` with --threads 1 and --threads N: the TSV (every fn, jt, br, call, win, pad, gap row and the summaries) and the
  printed lines the same; the same with --symbols-as-oracle;
- `nxaot plan` with --threads 1 and --threads N: the same convention facts;
- `nxaot build --cache` with --workers 1 and --workers N, each into a cache of its own: the analysis cache's entry (discovery's view and the
  plan, under the same key) and the shared object the same bytes.

With --baseline <nxaot> (another build, such as phase 5b's), its `nxaot discover` and `nxaot plan` outputs must be the same too.

  test_analysis_threads.py --nxaot <nxaot> --work <dir> [--threads N] [--module <path>]... [--baseline <nxaot>] [--no-build]

Exit 0 when every check holds.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
TOOLS_TESTS = os.path.join(HERE, '..', '..', 'tests')


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f'test_analysis_threads.py: {" ".join(cmd)} failed (exit {r.returncode}):\n'
                         f'{r.stdout.decode(errors="replace")}{r.stderr.decode(errors="replace")}')
    return r.stdout


def discover(nxaot, path, tsv, threads, oracle):
    cmd = [nxaot, 'discover', path, '--tsv', tsv] + (['--threads', str(threads)] if threads else []) + (['--symbols-as-oracle'] if oracle else [])
    return run(cmd), open(tsv, 'rb').read()


def check(a, name, path):
    """The checks of one module: a list of (what, same) and a short row"""
    out = []
    work = os.path.join(a.work, name)
    os.makedirs(work, exist_ok=True)
    for oracle in (False, True):
        mode = 'oracle' if oracle else 'symbols'
        one = discover(a.nxaot, path, os.path.join(work, f'{mode}.t1.tsv'), 1, oracle)
        many = discover(a.nxaot, path, os.path.join(work, f'{mode}.tN.tsv'), a.threads, oracle)
        out.append((f'discover ({mode})', one == many))
        if a.baseline:
            base = discover(a.baseline, path, os.path.join(work, f'{mode}.base.tsv'), 0, oracle)
            out.append((f'discover ({mode}) = baseline', base == one))
    plan1 = run([a.nxaot, 'plan', path, '--threads', '1'])
    planN = run([a.nxaot, 'plan', path, '--threads', str(a.threads)])
    out.append(('plan', plan1 == planN))
    if a.baseline:
        out.append(('plan = baseline', run([a.baseline, 'plan', path]) == plan1))
    if not a.no_build:
        packs = {}
        for workers in (1, a.threads):
            d = os.path.join(work, f'w{workers}')
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            so = os.path.join(d, 'pack.so')  # the same file name: the soname, written into the pack, is the same
            run([a.nxaot, 'build', path, '--shared', so, '--workers', str(workers), '--cache', os.path.join(d, 'cache')])
            entries = sorted(glob.glob(os.path.join(d, 'cache', 'analysis', '*.bin')))
            packs[workers] = (open(so, 'rb').read(), [(os.path.basename(e), open(e, 'rb').read()) for e in entries])
        (so1, an1), (soN, anN) = packs[1], packs[a.threads]
        out.append(('analysis cache entry', len(an1) == 1 and an1 == anN))
        out.append(('pack', so1 == soN))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--module', action='append', default=[])
    ap.add_argument('--baseline')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--no-corpus', action='store_true')
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    inputs = []
    if not a.no_corpus:
        sys.path.insert(0, TOOLS_TESTS)
        import aot_synth  # noqa: E402  (dm/tools/tests: the synthetic modules, written here as build_packs.sh writes them)
        aot_synth.write(os.path.join(a.work, 'synth', 'module'))
        aot_synth.write_hostfn(os.path.join(a.work, 'synth', 'hostfn'))
        with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
            for line in f:
                if not line.startswith('#') and line.strip():
                    nro = line.split('\t')[0]
                    inputs.append((nro[:-4], os.path.join(CORPUS, 'nro', nro)))
        inputs += [('module', os.path.join(a.work, 'synth', 'module', 'main.aot.tsv')),
                   ('hostfn-main', os.path.join(a.work, 'synth', 'hostfn', 'main.aot.tsv')),
                   ('hostfn-sub', os.path.join(a.work, 'synth', 'hostfn', 'sub.nro'))]
    for m in a.module:
        inputs.append((os.path.basename(os.path.dirname(os.path.abspath(m))) + '-' + os.path.basename(m).split('.')[0], m))
    failures = 0
    checks = 0
    print(f'{"module":<24} result (1 thread against {a.threads}{", and the baseline" if a.baseline else ""})')
    for name, path in inputs:
        results = check(a, name, path)
        checks += len(results)
        bad = [what for what, same in results if not same]
        failures += len(bad)
        print(f'{name:<24} {len(results)} outputs ' + ('identical' if not bad else 'DIFFER: ' + ', '.join(bad)))
    print(f'analysis on threads: {len(inputs)} modules, {checks} outputs compared, {failures} differ')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
