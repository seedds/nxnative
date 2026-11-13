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
BUILDS = PROFILE['scan_diagnostics']
ACQUISITION_BUILDS = PROFILE['scan_acquisition_diagnostics']
QUERY_BUILDS = PROFILE['scan_query_diagnostics']

def selected_builds(acquisition=False, query=False):
    if acquisition and query:
        raise ValueError('choose only one scan diagnostic')
    return QUERY_BUILDS if query else ACQUISITION_BUILDS if acquisition else BUILDS



def validate(module, rewritten=None, fixups=None, *, acquisition=False, query=False):
    """Reject an unknown full NSO ID, invalid offset or a changed/re-written probe instruction."""
    builds = selected_builds(acquisition, query)
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


def environment(module, *, acquisition=False, query=False):
    probes = validate(module, acquisition=acquisition, query=query)
    specs = [f'main:{off:x}' + (f'@x{reg}+{field:x}' if reg is not None else '') + (f'^s{scalar}' if scalar is not None else '')
             for _, off, _, reg, field, scalar in probes]
    spec = ','.join(specs)
    if len(spec) >= 512:
        raise ValueError('scan diagnostic list exceeds the host probe parser buffer')
    interval = 31 if query else 30  # Avoid repeatedly sampling the same slot of the three-slot query ring.
    cadence = f'DM_PROBE_CHANGES=0\nDM_PROBE_INTERVAL_FRAMES={interval}\nDM_PROBE_FLOATS=1\n' if acquisition or query else 'DM_PROBE_CHANGES=4\nDM_PROBE_INTERVAL_FRAMES=0\n'
    return f'DM_PROBE={spec}\n{cadence}DM_PROBE_EVERY=0\nDM_PROBE_LIMIT=512\nDM_PROBE_ALL=1\n'


def summarize(log, *, acquisition=False, query=False):
    """Report observed transitions, not inferred gameplay success; raw log retains full context."""
    probes = next(iter(selected_builds(acquisition, query).values()))
    cadence = f'polls sampled every {31 if query else 30} presented frames; event-only entries log every call' if acquisition or query else 'change-only observations'
    print(f'Scan diagnostics: {cadence}, at most 512 printed per site; absence after a cap is unknown.')
    for label, off, _, reg, field, scalar in probes:
        name = f'main+{off:#x}'
        # Older captures used the now-patched dimension load. Keep them readable,
        # but never install a trampoline on its PC-relative runtime branch.
        names = [name, 'main+0x336a8c'] if label == 'query-source-size' else [name]
        lines = re.findall(r'^\[dm\] PROBE (?:' + '|'.join(map(re.escape, names)) + r') #\d+ .*$', log, re.M)
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
                    if label == 'counter-binding-slot':
                        word = f'ring={struct.unpack_from("<Q", data)[0]}, selected={struct.unpack_from("<i", data, 8)[0]}'
                    elif label in ('query-source-size', 'query-size-accepted', 'query-copy-request'):
                        word = '{}x{}'.format(*struct.unpack_from('<II', data))
                    elif label == 'acquire-context-state':
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
    if query:
        size_lines = re.findall(r'^\[dm\] PROBE main\+0x(?:336a6c|336a8c) #\d+ .*$', log, re.M)
        sizes = set()
        for line in size_lines:
            match = re.search(r'\[x28\+0x14\]=([0-9a-f]{32})', line)
            if match:
                sizes.add(struct.unpack_from('<II', bytes.fromhex(match[1])))
        rejected = sorted((w, h) for w, h in sizes if not (120 <= w <= 1600 and 100 <= h <= 900))
        if rejected:
            print('  Observed source sizes outside the native scan-query bounds before adaptation: '
                  + ', '.join(f'{w}x{h}' for w, h in rejected) + ' (allowed width 120..1600, height 100..900).')
            adapted = bool(re.search(r'MP4 scan-source: (?:3200x1800|3840x2160) -> 1600x900', log))
            if adapted:
                print('  The runtime scan-source adapter is active after the source-size probe; inspect query-size-accepted and query-copy-request for its resulting dimensions.')
            else:
                print('  This establishes the size-guard rejection for those descriptors, not that all scan failures share this cause.')
        print('  Query source: native descriptor width/height; query-size-accepted means the size guard passed; query-copy-request also passed the normalized-position guards.')
        print('  CPU clear sites: x0=clear destination, x21=byte count; compare with GPU [kr] sbuf address.')
        print('  Copy sites: x1=mapped readback source; counter-consumed observes the selected CPU result slot.')
        print('  counter-binding-slot: selected=-1 skips adding the current counter resource; resource observations show its accepted parameter descriptor.')
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
    modes.add_argument('--query', action='store_true', help='source-size guard and CPU counter lifecycle')
    modes.add_argument('--acquisition', action='store_true', help='bounded upstream target-acquisition observations')
    modes.add_argument('--state', dest='acquisition', action='store_false', help='native scan-state observations (default)')
    parser.set_defaults(acquisition=False)
    sub = parser.add_subparsers(dest='action', required=True)
    env = sub.add_parser('env'); env.add_argument('original', type=Path)
    check = sub.add_parser('check'); check.add_argument('original', type=Path); check.add_argument('rewritten', type=Path)
    report = sub.add_parser('summary'); report.add_argument('log', type=Path)
    args = parser.parse_args()
    if args.action == 'summary':
        summarize(args.log.read_text(errors='replace'), acquisition=args.acquisition, query=args.query)
        return
    module = nso.load(args.original.read_bytes(), args.original.name)
    if args.action == 'env':
        print(environment(module, acquisition=args.acquisition, query=args.query), end='')
    else:
        probes = validate(module, args.rewritten.read_bytes(), layout(args.rewritten.with_name('main.fixups.tsv')), acquisition=args.acquisition, query=args.query)
        print(f'Scan diagnostics: original and rewritten main identity, layout and all {len(probes)} instructions verified.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        sys.exit(f'[RED] {error}')
