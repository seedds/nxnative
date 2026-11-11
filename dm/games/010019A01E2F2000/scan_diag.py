#!/usr/bin/env python3
"""Build-checked Scan Visor probes and an offline summary; never launches the game."""
import argparse
from pathlib import Path
import re
import runpy
import struct
import sys

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
import nso
from runtime_patch import layout

BUILDS = runpy.run_path(str(Path(__file__).with_name('patches.py')))['SCAN_DIAGNOSTICS']


def validate(module, rewritten=None, fixups=None):
    """Reject an unknown full NSO ID, invalid offset or a changed/re-written probe instruction."""
    build = module.build_id.hex()
    if build not in BUILDS:
        raise ValueError('scan diagnostics require the verified main v1.1.0 build')
    if rewritten is not None:
        if fixups is None or fixups.get('BUILD_ID') != build[:32] or int(fixups['TEXT_END'], 0) != module.text_end:
            raise ValueError('scan diagnostics: rewritten build/layout mismatch')
    probes = BUILDS[build]
    if len(probes) > 16 or len({p[1] for p in probes}) != len(probes):
        raise ValueError('scan diagnostics: invalid probe count/duplicate sites')
    for name, off, expected, reg, field, scalar in probes:
        before = bytes.fromhex(expected)
        if off < 0 or off % 4 or len(before) != 4 or off + 4 > module.text_end or (reg is not None and not 0 <= reg <= 28) or field < 0 or field > 0xffffff or (scalar is not None and not 0 <= scalar <= 31):
            raise ValueError(f'scan diagnostics: invalid site {name}')
        if bytes(module.image[off:off + 4]) != before:
            raise ValueError(f'scan diagnostics: original instruction mismatch at {off:#x}')
        if rewritten is not None and rewritten[off:off + 4] != before:
            raise ValueError(f'scan diagnostics: rewritten instruction mismatch at {off:#x}')
    return probes


def environment(module):
    probes = validate(module)
    specs = [f'main:{off:x}' + (f'@x{reg}+{field:x}' if reg is not None else '') + (f'^s{scalar}' if scalar is not None else '')
             for _, off, _, reg, field, scalar in probes]
    spec = ','.join(specs)
    if len(spec) >= 512:
        raise ValueError('scan diagnostic list exceeds the host probe parser buffer')
    return f'DM_PROBE={spec}\nDM_PROBE_CHANGES=4\nDM_PROBE_EVERY=0\nDM_PROBE_LIMIT=512\nDM_PROBE_ALL=1\n'


def summarize(log):
    """Report observed transitions, not inferred gameplay success; raw log retains full context."""
    probes = next(iter(BUILDS.values()))
    print('Scan diagnostics: change-only observations, at most 512 per site; absence after a cap is unknown.')
    for label, off, _, reg, field, scalar in probes:
        name = f'main+{off:#x}'
        lines = re.findall(r'^\[dm\] PROBE ' + re.escape(name) + r' #\d+ .*$', log, re.M)
        if not lines:
            print(f'  {label}: no printed hit (not proof that the function is absent)')
            continue
        frames = [int(v) for l in lines for v in re.findall(r' F=(\d+)', l)]
        words = []
        for line in lines:
            match = re.search(re.escape(f'[x{reg}+{field:#x}]=') + r'([0-9a-f]{32}|unreadable)', line)
            if match:
                word = 'unreadable' if match[1] == 'unreadable' else f'{struct.unpack_from("<I", bytes.fromhex(match[1]))[0]:08x}'
                if word not in words:
                    words.append(word)
        values = [float(v) for l in lines for v in re.findall(r' s\d+=([-+\d.e]+)\(', l)]
        span = f'f{min(frames)}..f{max(frames)}' if frames else 'frame unknown'
        extra = f'; scalar min/max {min(values):.6g}/{max(values):.6g}' if values else ''
        print(f'  {label}: {len(lines)} observations, {span}; first-word values {", ".join(words[:12]) or "none"}'
              + (' ...' if len(words) > 12 else '') + extra + ('; CAP REACHED' if len(lines) >= 512 else ''))
    print('  Fields: target-handle is an entity ID; target-resolved/scanning-target-state are target+0x2b0;')
    print('  target-range/begin-target/begin-range-gate expose flags at +0x2d8 (bit 2 = out of range);')
    print('  scanning-script-gate exposes script+0x3d0; begin-request logs every call; progress probes include the scalar argument.')
    print('  These observations do not establish that a scan completed; compare with input.rec and the HUD.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    env = sub.add_parser('env'); env.add_argument('original', type=Path)
    check = sub.add_parser('check'); check.add_argument('original', type=Path); check.add_argument('rewritten', type=Path)
    report = sub.add_parser('summary'); report.add_argument('log', type=Path)
    args = parser.parse_args()
    if args.action == 'summary':
        summarize(args.log.read_text(errors='replace'))
        return
    module = nso.load(args.original.read_bytes(), args.original.name)
    if args.action == 'env':
        print(environment(module), end='')
    else:
        validate(module, args.rewritten.read_bytes(), layout(args.rewritten.with_name('main.fixups.tsv')))
        print('Scan diagnostics: original and rewritten main identity, layout and all 11 instructions verified.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] {error}')
