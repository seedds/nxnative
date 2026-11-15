"""Build-checked, read-only HUD camera observations. Never launches gameplay."""
import argparse
import os
from pathlib import Path
import re
import runpy
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import nso
from runtime_patch import layout

PROFILE = runpy.run_path(str(Path(__file__).with_name('patches.py')))
BUILD = PROFILE['scan_build_id']
SITES = PROFILE['hud_layout_diagnostics'][BUILD]


def validate(module, rewritten=None, fixups=None):
    if module.build_id.hex() != BUILD:
        raise ValueError('HUD observations require the verified main v1.1.0 build')
    if rewritten is not None and (not fixups or fixups.get('BUILD_ID') != BUILD[:32]
            or int(fixups.get('TEXT_END', '0'), 0) != module.text_end):
        raise ValueError('HUD observations: rewritten build/layout mismatch')
    if len(SITES) > 16 or len({s[1] for s in SITES}) != len(SITES):
        raise ValueError('invalid HUD observation list')
    for label, off, expected, reg, field, scalar in SITES:
        before = bytes.fromhex(expected)
        if off < 0 or off % 4 or len(before) != 4 or off+4 > module.text_end or not 0 <= reg <= 28 or not 0 <= field <= 0xffffff or (scalar is not None and not 0 <= scalar <= 31):
            raise ValueError('invalid HUD observation site: ' + label)
        if module.image[off:off+4] != before or (rewritten is not None and rewritten[off:off+4] != before):
            raise ValueError('HUD observation instruction mismatch: ' + label)
    return SITES


def environment(module, menu=False, panels=False):
    sites = validate(module)
    if panels: sites=tuple(s for s in sites if s[1]!=0x916c0c)  # bounds branch is active in MENU_FIT=1610
    specs = ','.join(f'main:{off:x}@x{reg}+{field:x}' + (f'^s{scalar}' if scalar is not None else '')
                     for _, off, _, reg, field, scalar in sites)
    if len(specs) >= 512:
        raise ValueError('HUD observation list too long')
    interval = 120 if menu or panels else 600
    env = (f'DM_PROBE={specs}\nDM_PROBE_LIMIT=64\nDM_PROBE_EVERY=0\n'
           f'DM_PROBE_CHANGES=0\nDM_PROBE_INTERVAL_FRAMES={interval}\nDM_PROBE_FLOATS=1\nDM_PROBE_ALL=1\n')
    # This six-index draw was present only in the map frames of the supplied
    # pass dump. Its role as backdrop is NOT assumed: capture its projection
    # and vertex inputs to identify coverage before changing the renderer.
    if menu: env += 'DM_NVN_KEYDUMP=41368ee8d070c30a:6:120\n'
    return env


def summarize(log):
    out = ['HUD observations: read-only; see env.txt for DM_PROBE_INTERVAL_FRAMES (600 normally, 120 for menu observations), at most 64 printed entries.',
           'This does not change HUD size or establish which widgets use a camera. Missing/capped observations are unknown.']
    for label, off, _, reg, field, scalar in SITES:
        lines = re.findall(r'^\[dm\] PROBE main\+' + hex(off) + r' #\d+ .*$', log, re.M)
        values, contexts = [], set()
        for line in lines:
            memory = re.search(re.escape(f'[x{reg}+{field:#x}]=') + r'([0-9a-f]{32}|unreadable)', line)
            if memory and memory[1] != 'unreadable':
                raw = bytes.fromhex(memory[1])
                if label == 'hud-fov-context':
                    contexts.add(struct.unpack_from('<I', raw)[0])
                else:
                    value = struct.unpack('<I3f' if label == 'composition-camera' else '<4f', raw)
                    if value not in values: values.append(value)
            if scalar is not None:
                m = re.search(rf'(?:^| )s{scalar}=([^ (]+)\(', line)
                if m and m[1] not in values: values.append(m[1])
        detail = f'; contexts={sorted(contexts)}' if contexts else ''
        out.append(f'{label}: {len(lines)} printed entries{detail}; values={values[:8]}'
                   + ('; CAP REACHED' if len(lines) >= 64 else ''))
    quad = re.findall(r'^\[kd\] F(\d+) pass \d+ .*fs=41368ee8d070c30a[^ ]* .*$', log, re.M)
    out.append(f'Map quad input snapshots: {len(quad)} (limit 6); role/coverage require inspecting host.log, not inferring from the shader key.')
    return '\n'.join(out) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=('env', 'env-panels', 'check', 'summary'))
    p.add_argument('input', type=Path)
    p.add_argument('rewritten', type=Path, nargs='?')
    a = p.parse_args()
    if a.command == 'summary':
        print(summarize(a.input.read_text(errors='replace')), end=''); return
    module = nso.load(a.input.read_bytes(), 'main')
    if a.command in ('env','env-panels'):
        print(environment(module, menu=os.environ.get('MENU_LAYOUT_PROBE') == '1', panels=a.command=='env-panels'), end=''); return
    if a.rewritten is None: p.error('check requires the rewritten module')
    fixups = a.rewritten.with_name('main.fixups.tsv')
    validate(module, a.rewritten.read_bytes(), layout(fixups))
    print('PASS: HUD observation build/layout/instructions; no gameplay launched or HUD size changed.')


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError) as e:
        print('[RED] HUD observations: ' + str(e), file=sys.stderr); raise SystemExit(1) from None
