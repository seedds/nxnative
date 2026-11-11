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

PROFILE = runpy.run_path(str(Path(__file__).with_name('patches.py')))
BUILDS = PROFILE['SCAN_DIAGNOSTICS']
ACQUISITION_BUILDS = PROFILE['SCAN_ACQUISITION_DIAGNOSTICS']


def validate(module, rewritten=None, fixups=None, *, acquisition=False):
    """Reject an unknown full NSO ID, invalid offset or a changed/re-written probe instruction."""
    builds = ACQUISITION_BUILDS if acquisition else BUILDS
    build = module.build_id.hex()
    if build not in builds:
        raise ValueError('scan diagnostics require the verified main v1.1.0 build')
    if rewritten is not None:
        if fixups is None or fixups.get('BUILD_ID') != build[:32] or int(fixups['TEXT_END'], 0) != module.text_end:
            raise ValueError('scan diagnostics: rewritten build/layout mismatch')
    probes = builds[build]
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


def environment(module, *, acquisition=False):
    probes = validate(module, acquisition=acquisition)
    specs = [f'main:{off:x}' + (f'@x{reg}+{field:x}' if reg is not None else '') + (f'^s{scalar}' if scalar is not None else '')
             for _, off, _, reg, field, scalar in probes]
    spec = ','.join(specs)
    if len(spec) >= 512:
        raise ValueError('scan diagnostic list exceeds the host probe parser buffer')
    cadence = 'DM_PROBE_CHANGES=0\nDM_PROBE_INTERVAL_FRAMES=30\nDM_PROBE_FLOATS=1\n' if acquisition else 'DM_PROBE_CHANGES=4\nDM_PROBE_INTERVAL_FRAMES=0\n'
    return f'DM_PROBE={spec}\n{cadence}DM_PROBE_EVERY=0\nDM_PROBE_LIMIT=512\nDM_PROBE_ALL=1\n'


def summarize(log, *, acquisition=False):
    """Report observed transitions, not inferred gameplay success; raw log retains full context."""
    probes = next(iter((ACQUISITION_BUILDS if acquisition else BUILDS).values()))
    cadence = 'polls sampled every 30 presented frames; event-only entries log every call' if acquisition else 'change-only observations'
    print(f'Scan diagnostics: {cadence}, at most 512 printed per site; absence after a cap is unknown.')
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
                word = 'unreadable'
                if match[1] != 'unreadable':
                    data = bytes.fromhex(match[1])
                    word = f'{struct.unpack_from("<I", data)[0]:08x}'
                    if label == 'acquire-context-state':
                        word = f'flag134={data[4]:02x}'
                    elif label == 'acquire-context-pointer':
                        word = f'ptr190={struct.unpack_from("<Q", data)[0]:x}'
                if word not in words:
                    words.append(word)
        values = [float(v) for l in lines for v in re.findall(rf' s{scalar}=([-+\d.e]+)\(', l)] if scalar is not None else []
        span = f'f{min(frames)}..f{max(frames)}' if frames else 'frame unknown'
        extra = f'; scalar min/max {min(values):.6g}/{max(values):.6g}' if values else ''
        print(f'  {label}: {len(lines)} observations, {span}; first-word values {", ".join(words[:12]) or "none"}'
              + (' ...' if len(words) > 12 else '') + extra + ('; CAP REACHED' if len(lines) >= 512 else ''))
    if acquisition:
        print('  Acquisition: acquire-eligible passes the early context/input guard; spatial-candidates has candidate count in x1.')
        print('  raw-candidates / filtered-candidates: first word is the targeting list count before / after gameplay eligibility filtering.')
        print('  project-depth: s2 vs s8; project-bounds: target (s10,s9), min (s12,s11), max (s0,s1), all native projection coordinates.')
        print('  project-accepted reaches the rectangle-accept path; selected-candidate has the chosen component handle in x2.')
    print('  Fields: target-handle is an entity ID; target-resolved/scanning-target-state are target+0x2b0;')
    print('  target-range/begin-target/begin-range-gate expose flags at +0x2d8 (bit 2 = out of range);')
    print('  scanning-script-gate exposes script+0x3d0; begin-request logs every call; progress probes include the scalar argument.')
    print('  These observations do not establish that a scan completed; compare with input.rec and the HUD.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--acquisition', action='store_true', help='bounded upstream target-acquisition observations')
    modes.add_argument('--state', dest='acquisition', action='store_false', help='native scan-state observations (default)')
    parser.set_defaults(acquisition=False)
    sub = parser.add_subparsers(dest='action', required=True)
    env = sub.add_parser('env'); env.add_argument('original', type=Path)
    check = sub.add_parser('check'); check.add_argument('original', type=Path); check.add_argument('rewritten', type=Path)
    report = sub.add_parser('summary'); report.add_argument('log', type=Path)
    args = parser.parse_args()
    if args.action == 'summary':
        summarize(args.log.read_text(errors='replace'), acquisition=args.acquisition)
        return
    module = nso.load(args.original.read_bytes(), args.original.name)
    if args.action == 'env':
        print(environment(module, acquisition=args.acquisition), end='')
    else:
        probes = validate(module, args.rewritten.read_bytes(), layout(args.rewritten.with_name('main.fixups.tsv')), acquisition=args.acquisition)
        print(f'Scan diagnostics: original and rewritten main identity, layout and all {len(probes)} instructions verified.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] {error}')
