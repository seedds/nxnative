"""Summarize DM_NVN_TEX_BREAKDOWN snapshots without charging aliases twice.

The overwrite count is an estimate over entries still in the sharing table:
recorded GPU writes describe whole textures, not the pixels of a render area.
Heap bytes are releasable only if every observed member is a candidate; other
holders outside the sharing table can still keep that heap alive.
"""
import argparse
from collections import defaultdict
import json
import re


def overwritten(old, writer, frame):
    lo, hi = old['off'], old['off'] + old['guest']
    wl, wh = writer['off'], writer['off'] + writer['guest']
    return (old['written'] > 0 and frame + 1 >= old['written'] + 2
            and writer['written'] > old['written']
            and old['pool'] == writer['pool'] and lo != wl
            and wl < hi and wh > lo and not (wl >= lo and wh <= hi))


def union_bytes(ranges):
    total, end = 0, -1
    for lo, hi in sorted(ranges):
        total += max(0, hi - max(lo, end))
        end = max(end, hi)
    return total


def summarize(rows, frame):
    stats = defaultdict(lambda: defaultdict(int))
    ranges = defaultdict(list)
    allocations = defaultdict(list)
    textures = set()
    last_write = defaultdict(int)
    for row in rows:
        last_write[row['texture']] = max(last_write[row['texture']], row['written'])
    for row in rows:
        fmt = row['format']
        ranges[(fmt, row['pool'], row['samples'])].append(
            (row['off'], row['off'] + row['guest']))
        if row['texture'] in textures:
            continue
        textures.add(row['texture'])
        s = stats[fmt]
        s['textures'] += 1
        s['nominal'] += row['bytes']
        old = dict(row, written=last_write[row['texture']])
        dead = any(overwritten(old, other, frame) for other in rows)
        s['overwritten_nominal'] += row['bytes'] if dead else 0
        key = ('heap', row['heap']) if row['heap'] else ('texture', row['texture'])
        allocations[key].append((row, dead))
    for (fmt, _, _), spans in ranges.items():
        stats[fmt]['guest'] += union_bytes(spans)
    for members in allocations.values():
        row = members[0][0]
        mixed = len({r['format'] for r, _ in members}) > 1
        s = stats[-1 if mixed else row['format']]
        size = row['heap_bytes'] if row['heap'] else row['bytes']
        s['allocated'] += size
        s['family'] += size if row['family'] else 0
        s['releasable'] += size if all(dead for _, dead in members) else 0
        # The old pool heaps can span formats: report their charge separately.
        s['mixed_heap'] += size if mixed else 0
    return {fmt: dict(values) for fmt, values in stats.items()}


def snapshots(lines):
    frame, rows = None, []
    for line in lines:
        if '[rtbreakdown]' not in line:
            continue
        values = {k: int(v, 0) for k, v in re.findall(r'(\w+)=(0x[0-9a-f]+|\d+)', line)}
        if 'begin' in values:
            frame, rows = values['frame'], []
        elif 'end' in values and frame is not None:
            yield frame, rows
            frame = None
        elif 'texture' in values and frame is not None:
            rows.append(values)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()
    with open(args.log, errors='replace') as f:
        reports = [(frame, summarize(rows, frame)) for frame, rows in snapshots(f)]
    if not reports:
        ap.error('no complete render-target snapshots found')
    if args.json:
        print(json.dumps(reports))
        return
    peak = max(reports, key=lambda r: sum(s.get('allocated', 0) for s in r[1].values()))
    for label, (frame, report) in [('peak', peak), ('last', reports[-1])]:
        print(f'{label} frame {frame}: MiB, GPU-only shared targets; overwrite is an estimate')
        print('format -1 charges a heap shared by multiple formats once, outside the per-format totals')
        print('format count nominal allocated guest_union family_heap overwritten releasable mixed_heap')
        for fmt, s in sorted(report.items()):
            keys = ('nominal', 'allocated', 'guest', 'family', 'overwritten_nominal', 'releasable', 'mixed_heap')
            print(fmt, s.get('textures', 0), *(f'{s.get(k, 0) / 2**20:.2f}' for k in keys))


if __name__ == '__main__':
    main()
