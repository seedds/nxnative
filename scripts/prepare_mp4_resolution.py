#!/usr/bin/env python3
"""Prepare full-output MP4 modes using existing app assets; no shader compiler or gameplay.

Without --prepare/--repack this is a read-only preflight. --repack also replaces
only the requested app after successful packing. Saves remain outside the app.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(REPO / 'scripts'), str(REPO / 'dm/tools'), str(REPO / 'dm/games')]
import nso
import runtime_patch
import repack_macos_app


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('app', type=Path); p.add_argument('--nsp', type=Path, required=True)
    p.add_argument('--prepare', action='store_true'); p.add_argument('--repack', action='store_true')
    a = p.parse_args(argv)
    source = repack_macos_app.inputs(a.app); app, info, g, _, dm, _, _ = source
    if g.id != '010019A01E2F2000': raise ValueError('this preparation is only for Metroid Prime 4: Beyond')
    nsp = a.nsp.expanduser().resolve(); build = nsp / (g.id + '_build')
    original = build / 'extract/v131072/ExeFS/main'
    spec = importlib.util.spec_from_file_location('mp4_resolution', g.dir / 'runtime_patches.py')
    assert spec and spec.loader
    rt = importlib.util.module_from_spec(spec); spec.loader.exec_module(rt)
    module = nso.load(original.read_bytes(), original.name)
    if module.build_id.hex() != rt.BUILD: raise ValueError('requires the exact registered MP4 v1.1.0 main build')
    builds = g.builds()['v131072']
    for name in g.words('modules'):
        bid = builds[name]
        if runtime_patch.layout(dm / (name + '.fixups.tsv')).get('BUILD_ID') != bid:
            raise ValueError('source app module build mismatch: ' + name)
    control = build / 'extract/v131072/control.nacp'
    if not control.is_file(): raise ValueError('missing extracted control.nacp')
    if not (a.prepare or a.repack):
        print('PASS: exact game build and compatible full app/shader assets; no files changed or game launched.'); return 0
    out = nsp / (g.id + '-work/resolution-output-v2'); patched = out / 'patched'; patched.mkdir(parents=True, exist_ok=True)
    # Copy only small module/rewriter tables. Never duplicate RomFS/shader images.
    for pattern in ('*.patched.bin', '*.fixups.tsv', '*.gates.tsv', '*.cuts.tsv', '*.obs.tsv', '*.changed.tsv', '*.runtime.tsv', 'nro.tsv'):
        for src in dm.glob(pattern): shutil.copyfile(src, patched / src.name)
    env = dict(os.environ, DM_GAME=g.id, SETTINGS_PLATFORM='mac', GFXAPI='nvn', DM_LANG=info.get('DMLanguage', 'en-US'))
    with (out / 'rewrite.log').open('w') as log:
        subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(original), str(patched), 'main'], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    image = patched / 'main.patched.bin'
    (patched / 'main.runtime.tsv').write_text(rt.generate(module, image.read_bytes(), runtime_patch.layout(patched / 'main.fixups.tsv')))
    listed = out / 'launcher.json'
    subprocess.run([sys.executable, str(REPO / 'dm/games/settings.py'), g.id, str(patched), str(control), str(listed)], env=env, check=True)
    data = json.loads(listed.read_text())
    row = next((s for s in data['settings'] if s['env'] == 'DM_INTERNAL_RESOLUTION'), None)
    if not row or row['default'] != '1800p': raise ValueError('resolution menu or stable default missing')
    print(f'Prepared modules and resolution menu: {out}\nNo game shaders or offline pipelines compiled; no game launched.', flush=True)
    if a.repack:
        subprocess.run([sys.executable, str(REPO / 'scripts/repack_macos_app.py'), str(app), '--patched', str(patched), '--settings', str(listed), '--resolution-menu', '--without-bundled-pso', '--skip-pipeline-prebuild'], check=True)
    return 0


if __name__ == '__main__':
    try: raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'[RED] MP4 resolution preparation: {e}', file=sys.stderr); raise SystemExit(1) from None
