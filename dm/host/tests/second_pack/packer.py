#!/usr/bin/env python3
"""The packer CTest t_second_pack (dm/host/tests/t_second_pack.py) names in its launch file's `packer` row: what scripts/build_ipa_from_nsp.py --platform linux / windows does
on a repack of a game whose recompiler's input is already there, without the NSP, the keys, the extraction and the rewrite (the synthetic module has none of them). It runs the
packer's own code for the rest, imported from scripts/build_ipa_from_nsp.py (its import has no side effect: main runs only from cli):
  nxaot_options   the options this nxaot has (its `nxaot build` usage)
  aot_packs       the stage "AOT packs": `nxaot build <name>.aot.tsv --shared <name>.aot.so --mode <mode> [--cache] [--profile <profile>/<name>.tsv] [--misses <profile>/<name>.misses.tsv]`
                  per module, the pack reused when its key (the manifest, nxaot, the mode, the profile's bytes, the misses' pairs) is as at its last build; nxaot's log in
                  <id>_build/logs/nxaot_<name>.log
  made_rows       the launch file's made, samples and misses rows: what the packs took
  packs_ready     the pack modes whose packs are complete and current: the launch file's pack rows
and writes the launch file again as port_launch_file orders its rows (game, path, made / samples / misses, pack, env), its other rows as they were.
  packer.py <NSP folder> --platform linux|windows --pack-mode balanced|max [-k <prod.keys>] [--nxaot <nxaot>]
the arguments the launcher's repack passes (dm/launcher/launcher_model.cpp repack_command). The game: the one <id>_build/ in the NSP folder with a launch file of the platform;
its recompiler's input: the balanced pack row's folder (aot_<version>/, pack_dir), its modules: the manifests there; the profile folder: the launch file's profile row. Exit 0
when the packs and the launch file are written, 1 on an error ([RED] line), 2 on a usage error"""
import argparse
import importlib.util
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)


def rows_of(path):   # the launch file: its '#' lines and its (kind, name, value) rows, in order
    head, rows = [], []
    lines = path.read_text(encoding='utf-8').splitlines()
    if not lines or lines[0] != nsp.LAUNCH_HEAD: raise nsp.BuildError(f'{path} is not a launch file of version 1 ({nsp.LAUNCH_HEAD})')
    for line in lines:
        if not line or line.startswith('#'): head.append(line); continue
        k, n, v = (line.split('\t', 2) + ['', ''])[:3]
        rows.append((k, n, v))
    return head, rows


def repack(a):
    platform, mode = a.platform, a.pack_mode
    builds = sorted(p for p in Path(a.package).iterdir() if p.name.endswith('_build') and (p / f'launch-{platform}.tsv').is_file())
    if len(builds) != 1: raise nsp.BuildError(f'{a.package} holds {len(builds)} <id>_build folders with a launch-{platform}.tsv, not one')
    work = builds[0]
    launch = work / f'launch-{platform}.tsv'
    head, rows = rows_of(launch)
    paths = {n: v for k, n, v in rows if k == 'path'}
    packs = {n: v for k, n, v in rows if k == 'pack'}
    if 'balanced' not in packs or not paths.get('profile') or not paths.get('cwd'): raise nsp.BuildError(f'{launch} names no balanced pack, profile or cwd')
    aot = Path(packs['balanced'])   # pack_dir(aot, 'balanced') is the recompiler's input itself
    pkg_ver = aot.name.split('_', 1)[1]
    nro_dir, wdir = work / f'nro_{pkg_ver}', Path(paths['cwd']).parent
    mods = sorted(p.name[:-len('.aot.tsv')] for p in aot.glob('*.aot.tsv'))
    profiles = misses = Path(paths['profile'])   # as main: <id>-work/profile, the sample profiles and the JIT misses
    nxaot = Path(a.nxaot or paths.get('nxaot', ''))
    env = dict(os.environ)
    opts = nsp.nxaot_options(nxaot, env)
    nsp.say(f'packer stand-in: {work.name}: modules {", ".join(mods)} from {aot}; nxaot {nxaot} (options {", ".join(sorted(opts)) or "none"}); profile folder {profiles}')
    built = nsp.aot_packs(nxaot, opts, mode, 0, wdir / 'aot-cache', aot, nro_dir, mods, env, work / 'logs', work / 'tmp/aot', platform, profiles, misses)
    for m in mods:
        log = work / 'logs' / f'nxaot_{m}.log'
        if built and log.is_file():
            for line in log.read_text(errors='replace').splitlines():
                if line.startswith(('aot: misses', '[RED]', 'aot: profile')) or 'gate sites' in line: nsp.say(f'nxaot_{m}.log: {line}')
    made = nsp.made_rows(opts, aot, mods, profiles, misses, False)
    ready = sorted(nsp.packs_ready(nxaot, opts, aot, nro_dir, mods, platform, profiles, misses), key=lambda m: m != mode)   # the mode just packed first, as main
    # port_launch_file's order: game and path rows, the made rows, the pack rows, the env rows; the rows this run writes replace theirs
    keep = [r for r in rows if r[0] not in ('made', 'samples', 'misses', 'pack')]
    at = max((i + 1 for i, r in enumerate(keep) if r[0] in ('game', 'path')), default=0)
    out = keep[:at] + made + [('pack', m, str(nsp.pack_dir(aot, m))) for m in ready] + keep[at:]
    text = '\n'.join([h for h in head if h] + ['\t'.join(nsp.launch_field(x, f'the launch file\'s {r[0]} row {r[1]}') for x in r) for r in out]) + '\n'
    tmp = launch.with_name(launch.name + '.partial'); tmp.write_bytes(text.encode('utf-8')); os.replace(tmp, launch)
    nsp.rmtree(work / 'tmp')
    for r in made: nsp.say('launch file: ' + ' '.join(r))
    nsp.say(f'packer stand-in: {built} packs built ({mode}); {launch} offers {", ".join(ready) or "no mode"}')


def main(argv):
    ap = argparse.ArgumentParser(description='the repack of t_second_pack: the packer\'s pack stage and launch file rows on a prepared <id>_build')
    ap.add_argument('package')
    ap.add_argument('--platform', choices=('linux', 'windows'), required=True)
    ap.add_argument('--pack-mode', choices=nsp.PACK_MODES, default='balanced')
    ap.add_argument('-k', dest='keys', default='')   # the launcher passes the keys row: not read (nothing is extracted)
    ap.add_argument('--nxaot', default='')
    a = ap.parse_args(argv)
    try:
        repack(a)
    except nsp.BuildError as e:
        nsp.PROG.close(ok=False); nsp.say(f'[RED] {e}'); return 1
    nsp.PROG.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
