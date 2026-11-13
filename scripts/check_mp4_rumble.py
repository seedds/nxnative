#!/usr/bin/env python3
"""Validate an MP4 rumble candidate; --run repacks and starts exactly one 1800p session."""
import argparse
import datetime
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(REPO / 'scripts'), str(REPO / 'dm/tools'), str(REPO / 'dm/games/010019A01E2F2000')]
import nso
import repack_macos_app
_spec = importlib.util.spec_from_file_location('mp4_rumble_diag', REPO / 'dm/games/010019A01E2F2000/rumble_diag.py')
assert _spec and _spec.loader
rumble_diag = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(rumble_diag)
from runtime_patch import layout


def preflight(app, nsp):
    source = repack_macos_app.inputs(app)
    if source[2].id != '010019A01E2F2000': raise ValueError('this checkpoint is only for Metroid Prime 4: Beyond')
    dm = source[4]
    original = nsp / '010019A01E2F2000_build/extract/v131072/ExeFS/main'
    probes = rumble_diag.validate(nso.read(original), (dm / 'main.patched.bin').read_bytes(), layout(dm / 'main.fixups.tsv'))
    return probes


def run_environment(probes, out):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DM_PROBE') and k not in
           {'DM_KEYS', 'DM_CTL', 'DM_REC', 'DM_AUDIO_DUMP', 'DM_AUDREN_PCM_TRACE', 'DM_NVN_PASSDUMP'}}
    env.update(probes)
    env.update(DM_RUMBLE='1', DM_RUMBLE_GAIN='1', DM_RUMBLE_MIN='0', DM_RUMBLE_LOG='1',
               DM_LAUNCHER='0', DM_INTERNAL_RESOLUTION='1800p', DM_PRESENT_UPSCALE='off', DM_METAL_HUD='0',
               DM_PSO_BUNDLE='off', DM_PSO_PREBUILD='0', DM_REC=str(out / 'input.rec'))
    return env


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('app', type=Path); p.add_argument('--nsp', type=Path, required=True)
    p.add_argument('--run', action='store_true', help='replace the app using its existing shaders, then launch exactly once')
    a = p.parse_args(argv); app = a.app.expanduser().resolve(); nsp = a.nsp.expanduser().resolve()
    probes = preflight(app, nsp)
    if not a.run:
        print('PASS: matching game, shader stamp, main build/layout and six read-only rumble probe instructions. No build or game launch.')
        return 0
    print('Close any running game. This checkpoint repacks the existing app, launches 1800p once and records manual play.', flush=True)
    subprocess.run([sys.executable, str(REPO / 'scripts/repack_macos_app.py'), str(app), '--without-bundled-pso', '--skip-pipeline-prebuild'], check=True)
    probes = preflight(app, nsp)
    out = nsp / '010019A01E2F2000-work/analysis' / ('rumble-check-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=False)
    print(f'Diagnostics: {out}\nScenario: fire 5–10 shots during normal gameplay, then quit. Report whether either side vibrated.', flush=True)
    result = subprocess.run([str(app / 'Contents/MacOS/nxnative')], env=run_environment(probes, out))
    log = Path.home() / 'Library/Logs/nxnative/010019A01E2F2000.log'
    if log.is_file():
        shutil.copyfile(log, out / 'host.log')
        summary = rumble_diag.summarize(log.read_text(errors='replace'))
        (out / 'summary.txt').write_text(summary); print(summary)
    else: print('[RED] launcher log not found; the checkpoint is incomplete', file=sys.stderr)
    print(f'Diagnostics: {out}'); return result.returncode


if __name__ == '__main__':
    try: raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'[RED] rumble checkpoint: {e}', file=sys.stderr); raise SystemExit(1) from None
