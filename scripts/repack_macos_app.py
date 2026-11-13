#!/usr/bin/env python3
"""Rebuild a Mac host/launcher and repack an existing NVN app; never compile game shaders."""
import argparse
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools'), str(REPO / 'dm/games')]
import dmsh_chain
import game
import shader_lib


def inputs(app):
    app = app.expanduser().resolve()
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    title = info.get('DMTitleID', '')
    if not re.fullmatch(r'[0-9A-Fa-f]{16}', title):
        raise ValueError('source app has no valid DMTitleID')
    g = game.load(title.upper())
    chain = re.search(r'#define DMSH_CHAIN "([0-9a-f]+)"', (REPO / 'dm/gfx/nvn/dmsh_chain.h').read_text())
    if not chain or info.get('DMChain') != chain[1]:
        raise ValueError('source shaders do not match the current host chain; use the normal build script')
    res = app / 'Contents/Resources'; dm = res / 'dm'; lib = res / 'msl' / ('b-' + chain[1])
    stamp = shader_lib.read_stamp(lib)
    if stamp.get('game') != g.id or stamp.get('inputs') != dmsh_chain.inputs(g):
        raise ValueError('source app needs a full shader stamp matching the current game profile')
    rom = res / 'romfs.img' if (res / 'romfs.img').is_file() else res / 'romfs'
    if not rom.exists():
        raise ValueError('source app has no RomFS')
    return app, info, g, res, dm, lib, rom


def environment(dm, without_pso, skip_prebuild, resolution_menu=False):
    p = dm / 'env.txt'
    lines = p.read_text().splitlines() if p.is_file() else []
    overrides = {}
    if without_pso: overrides['DM_PSO_BUNDLE'] = 'off'
    if skip_prebuild: overrides['DM_PSO_PREBUILD'] = '0'
    removed = set(overrides) | ({'DM_INTERNAL_RESOLUTION', 'DM_LAUNCHER'} if resolution_menu else set())
    lines = [line for line in lines if line.partition('=')[0].strip() not in removed]
    return '\n'.join(lines + [f'{k}={v}' for k, v in overrides.items()]) + '\n'


def command(source, out, env, patched=None, settings=None):
    app, info, g, res, dm, lib, rom = source
    cmd = [sys.executable, str(REPO / 'dm/mac/pack_app.py'), str(out),
           '--host', str(REPO / 'dm/host/dmhost'), '--patched', str(patched or dm), '--romfs', str(rom),
           '--msl', str(lib), '--tables', str(dm), '--font', str(dm / 'font'),
           '--title-id', g.id, '--name', info.get('CFBundleDisplayName', app.stem), '--env-file', str(env)]
    for flag, value in (('--bundle-id', info.get('CFBundleIdentifier')), ('--version', info.get('CFBundleShortVersionString')),
                        ('--category', info.get('LSApplicationCategoryType')), ('--lang', info.get('DMLanguage')),
                        ('--marker', info.get('DMRomfsMarker'))):
        if value: cmd += [flag, str(value)]
    for flag, path in (('--settings', settings or dm / 'launcher.json'), ('--html', res / 'html'),
                       ('--aoc', res / 'aoc'), ('--icon', res / 'AppIcon.icns')):
        if path.exists(): cmd += [flag, str(path)]
    return cmd


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('app', type=Path, help='existing complete NVN app; replaced only after successful packing')
    ap.add_argument('--out', type=Path, help='output app (default: replace the source app)')
    ap.add_argument('--without-bundled-pso', action='store_true', help='omit its offline pipeline archive and disable bundle lookups')
    ap.add_argument('--skip-pipeline-prebuild', action='store_true', help='disable the full pair-list prebuild; keep normal prewarm and draw-time compilation')
    ap.add_argument('--patched', type=Path, help='use separately prepared rewritten modules')
    ap.add_argument('--settings', type=Path, help='use separately prepared launcher settings')
    ap.add_argument('--resolution-menu', action='store_true', help='let the launcher choose resolution instead of a fixed bundled environment value')
    ap.add_argument('-j', '--jobs', type=int, default=8)
    a = ap.parse_args(argv)
    source = inputs(a.app)
    app, info, g, res, dm, lib, rom = source
    available = ()
    if a.patched:
        import runtime_patch
        available = runtime_patch.choices(a.patched, 'DM_INTERNAL_RESOLUTION')
        if not available:
            raise ValueError('replacement modules need a complete matching runtime manifest')
    if a.settings:
        import json
        listed = json.loads(a.settings.read_text())
        if not isinstance(listed.get('settings'), list): raise ValueError('invalid launcher settings')
        if a.patched:
            row = next((s for s in listed['settings'] if s['env'] == 'DM_INTERNAL_RESOLUTION'), None)
            if not row or row['default'] not in available or any(o[0] not in available for o in row['options']):
                raise ValueError('resolution menu does not match replacement modules')
    if a.resolution_menu and not (a.patched and a.settings):
        raise ValueError('resolution menu requires prepared modules and settings')
    if (dm / 'programs.tsv').exists():
        raise ValueError('multi-program apps need the normal build script')
    out = a.out.expanduser().resolve() if a.out else app
    if app in out.parents:
        raise ValueError('output cannot be inside the source app')
    print(f'Reusing complete game/shader assets from {app}; no game shader or offline pipeline compiler runs.', flush=True)
    subprocess.run(['make', '-C', str(REPO / 'dm/host'), 'GAME=' + g.id, '-j' + str(max(1, a.jobs))], check=True)
    with tempfile.TemporaryDirectory(prefix='nxnative-repack-') as tmp:
        env = Path(tmp) / 'env.txt'
        env.write_text(environment(dm, a.without_bundled_pso, a.skip_pipeline_prebuild, a.resolution_menu))
        cmd = command(source, out, env, a.patched, a.settings)
        if not a.without_bundled_pso and (res / 'pso').is_dir(): cmd += ['--pso', str(res / 'pso')]
        subprocess.run(cmd, check=True)
    print(f'App: {out}. Saves stay in ~/Library/Application Support/nxnative/{g.id}/', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'[RED] repack: {e}', file=sys.stderr)
        raise SystemExit(1) from None
