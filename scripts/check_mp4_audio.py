#!/usr/bin/env python3
"""Validate the audio candidate; --run repacks and starts one 1800p cutscene checkpoint."""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(REPO / 'scripts'), str(REPO / 'dm/tools')]
import audio_trace
import repack_macos_app


def preflight(app):
    source = repack_macos_app.inputs(app)
    if source[2].id != '010019A01E2F2000': raise ValueError('this checkpoint is only for Metroid Prime 4: Beyond')


def run_environment(out):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DM_PROBE') and k not in
           {'DM_KEYS', 'DM_CTL', 'DM_REC', 'DM_AUDIO_DUMP', 'DM_AUDREN_PCM_TRACE', 'DM_AUDREN_TRACE', 'DM_AUDREN_AUX_TRACE', 'DM_NVN_PASSDUMP', 'DM_RUMBLE_LOG'}}
    env.update(DM_LAUNCHER='0', DM_INTERNAL_RESOLUTION='1800p', DM_PRESENT_UPSCALE='off', DM_METAL_HUD='0',
               DM_PSO_BUNDLE='off', DM_PSO_PREBUILD='0', DM_RUMBLE='1', DM_RUMBLE_GAIN='1', DM_RUMBLE_MIN='0',
               DM_REC=str(out / 'input.rec'), DM_AUDIO_DUMP=str(out / 'game-stereo.pcm'),
               DM_AUDREN_PCM_TRACE=str(out / 'frames.tsv'), DM_AUDREN_AUX_TRACE='0')
    return env


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('app', type=Path); p.add_argument('--nsp', type=Path, required=True)
    p.add_argument('--run', action='store_true', help='replace the app using its existing shaders, then launch exactly once')
    a = p.parse_args(argv); app = a.app.expanduser().resolve(); nsp = a.nsp.expanduser().resolve()
    preflight(app)
    if a.run and not nsp.is_dir(): raise ValueError('NSP folder is not a directory')
    if not a.run:
        print('PASS: compatible existing MP4 app and shader inputs. No build, file replacement or game launch.')
        return 0
    print('Close the running game. Repacking only the host/launcher with existing shader assets.', flush=True)
    subprocess.run([sys.executable, str(REPO / 'scripts/repack_macos_app.py'), str(app), '--without-bundled-pso', '--skip-pipeline-prebuild'], check=True)
    preflight(app)
    out = nsp / '010019A01E2F2000-work/analysis' / ('audio-fifo-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=False)
    print(f'Diagnostics: {out}\nOne 1800p launch. Watch one cutscene that previously crackled, then quit. Rumble stays enabled; guest probes and detailed Aux tracing are off.', flush=True)
    result = subprocess.run([str(app / 'Contents/MacOS/nxnative')], env=run_environment(out))
    log = Path.home() / 'Library/Logs/nxnative/010019A01E2F2000.log'
    if log.is_file(): shutil.copyfile(log, out / 'host.log')
    pcm = out / 'game-stereo.pcm'; frames = out / 'frames.tsv'
    if pcm.is_file() and frames.is_file():
        report = audio_trace.analyze(frames, pcm); report['pcm'] = audio_trace.pcm_summary(pcm)
        (out / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    else: print('[RED] audio capture incomplete; preserve host.log', file=sys.stderr)
    print(f'Diagnostics: {out}'); return result.returncode


if __name__ == '__main__':
    try: raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'[RED] audio checkpoint: {e}', file=sys.stderr); raise SystemExit(1) from None
