"""Read-only, build-checked vibration probes; no game behaviour patch or launch."""
import math
import re
import runpy
from pathlib import Path
import struct

PROFILE = runpy.run_path(str(Path(__file__).with_name('patches.py')))
BUILD = PROFILE['SCAN_BUILD_ID']
SITES = PROFILE['RUMBLE_DIAGNOSTICS'][BUILD]


def validate(module, rewritten, fixups):
    if module.build_id.hex() != BUILD or fixups.get('BUILD_ID') != BUILD[:32] or int(fixups.get('TEXT_END', '0'), 0) != module.text_end:
        raise ValueError('rumble probes require the verified main v1.1.0 and its matching rewritten layout')
    for label, off, expected, reg, field in SITES:
        before = bytes.fromhex(expected)
        if off < 0 or off % 4 or len(before) != 4 or off + 4 > module.text_end or (reg is not None and not 0 <= reg <= 28) or field < 0:
            raise ValueError(f'invalid rumble probe site: {label}')
        if bytes(module.image[off:off + 4]) != before or rewritten[off:off + 4] != before:
            raise ValueError(f'rumble probe instruction mismatch: {label}')
    spec = ','.join(f'main:{off:x}' + (f'@x{reg}+{field:x}' if reg is not None else '') for _, off, _, reg, field in SITES)
    if len(spec) >= 512 or len(SITES) > 16 or len({s[1] for s in SITES}) != len(SITES):
        raise ValueError('invalid rumble probe list')
    return {'DM_PROBE': spec, 'DM_PROBE_CHANGES': '16', 'DM_PROBE_LIMIT': '512', 'DM_PROBE_EVERY': '0',
            'DM_PROBE_INTERVAL_FRAMES': '0', 'DM_PROBE_FLOATS': '1', 'DM_PROBE_ALL': '1'}


def summarize(log):
    result = ['Rumble candidate: SDK handle construction; gameplay confirmation requires the player.',
              'Memory probes print changes only, at most 512 per site; absence/capped output is unknown.']
    stats = re.findall(r'rumble: (\d+) values sent, (\d+) with no output, peak amplitude ([\w.+-]+), handle ([^\s,]+)([^\n]*)', log)
    if stats:
        result.append(f'Input values: {sum(int(s[0]) for s in stats)}; unrouted (including silence): {sum(int(s[1]) for s in stats)}.')
        peaks = [float(s[2]) for s in stats]
        result.append(f'Largest finite logged amplitude: {max((v for v in peaks if math.isfinite(v)), default=0):g}.')
        for label in ['nonzero', 'unrouted nonzero', 'nonfinite']:
            counts = [int(m[1]) for s in stats if (m := re.search(r'(?:^|, )' + label + r' (\d+)', s[4]))]
            result.append(f'{label}: {sum(counts) if counts else "not recorded"}.')
        result.append('The final partial reporting second is not included in these totals.')
    for label, off, _, reg, field in SITES:
        lines = re.findall(r'^\[dm\] PROBE main\+' + hex(off) + r' #\d+ .*$', log, re.M)
        detail = ''
        if label in ['source-curve-left', 'request-left']:
            vectors = [struct.unpack('<4f', bytes.fromhex(m[1])) for l in lines if (m := re.search(r'\[x' + str(reg) + r'\+' + hex(field) + r'\]=([0-9a-f]{32})', l))]
            # These vectors are low amplitude/frequency then high amplitude/frequency.
            detail = f'; finite positive amplitude observations: {sum(any(math.isfinite(v[k]) and v[k] > 0 for k in [0, 2]) for v in vectors)}'
        result.append(f'{label}: {len(lines)} printed observations{detail}' + ('; cap reached' if len(lines) >= 512 else ''))
    return '\n'.join(result) + '\n'
