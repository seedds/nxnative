#!/usr/bin/env python3
"""The compile workers bounded by memory (CTest `workers`; README.md, "Memory at scale"): with --workers 0 (the default) `nxaot build` sizes its
pool of compile workers when the analysis is done, by the memory then available (MemAvailable on Linux, GlobalMemoryStatusEx on Windows; the
test switch NXAOT_MEM_AVAILABLE_MIB stands for it) less what the tables' object takes at the module's size, over the measured peak of one
worker at the units' tiers: at most one per CPU this process may run on and one per unit, at least one. It logs
`aot: workers N (M MiB available, about K MiB each, T MiB for the tables)`; --workers N takes N and logs `aot: workers N (--workers)`.

For a corpus NRO and the synthetic game module: a small MemAvailable gives one worker; one that holds three workers' and the tables' gives
min(3, CPUs, units + 1); a large one gives min(CPUs, units + 1); --workers 3 gives 3; every pack the same bytes (the workers never change a pack).

  test_workers.py --nxaot <nxaot> --work <dir>

Exit 0 when every check holds.
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
LINE = re.compile(r'^aot: workers (\d+) \((?:(\d+) MiB available, about (\d+) MiB each, (\d+) MiB for the tables|(--workers))\)$', re.M)
UNITS = re.compile(r'^aot: units (\d+) ', re.M)


def build(nxaot, module, so, env_mib=None, workers=None):
    env = dict(os.environ)
    env.pop('NXAOT_MEM_AVAILABLE_MIB', None)
    if env_mib is not None:
        env['NXAOT_MEM_AVAILABLE_MIB'] = str(env_mib)
    cmd = [nxaot, 'build', module, '--shared', so] + (['--workers', str(workers)] if workers else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit(f'test_workers.py: {" ".join(cmd)} failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    m = LINE.search(r.stdout)
    u = UNITS.search(r.stdout)
    if not m or not u:
        raise SystemExit(f'test_workers.py: no `aot: workers` or `aot: units` line in the log of {" ".join(cmd)}:\n{r.stdout}')
    return m, int(u.group(1)), open(so, 'rb').read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    sys.path.insert(0, TOOLS_TESTS)
    import aot_synth  # noqa: E402  (dm/tools/tests: the synthetic game module, as build_packs.sh writes it)
    aot_synth.write(os.path.join(a.work, 'module'))
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count()
    modules = [('fnptr-gcc', os.path.join(CORPUS, 'nro', 'fnptr-gcc.nro')), ('module', os.path.join(a.work, 'module', 'main.aot.tsv'))]
    failures = 0
    for name, path in modules:
        so = os.path.join(a.work, name + '.so')
        rows = []
        # Small: one worker
        m, units, pack = build(a.nxaot, path, so, env_mib=1)
        each, tables = int(m.group(3)), int(m.group(4))
        rows.append(('MemAvailable 1 MiB', int(m.group(1)), 1, m.group(0), pack))
        # Room for three workers and the tables
        mib = tables + 3 * each + each // 2
        m, units, pack = build(a.nxaot, path, so, env_mib=mib)
        rows.append((f'MemAvailable {mib} MiB', int(m.group(1)), max(1, min(3, cpus, units + 1)), m.group(0), pack))
        # Plenty
        m, units, pack = build(a.nxaot, path, so, env_mib=1 << 20)
        rows.append(('MemAvailable 1 TiB', int(m.group(1)), max(1, min(cpus, units + 1)), m.group(0), pack))
        # --workers overrides
        m, units, pack = build(a.nxaot, path, so, workers=3)
        rows.append(('--workers 3', int(m.group(1)), min(3, units + 1), m.group(0), pack))
        first = rows[0][4]
        for what, got, want, line, pack in rows:
            ok = got == want and pack == first
            failures += not ok
            print(f'{name:<10} {what:<22} {line}{"" if got == want else f"  FAIL: want {want} workers"}'
                  f'{"" if pack == first else "  FAIL: another pack"}')
    print(f'workers bounded by memory: {len(modules)} modules, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
