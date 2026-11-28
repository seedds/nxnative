#!/usr/bin/env python3
"""Join bounded SyncWait events by guest wait identity (CPU clock only)."""
import argparse
import csv
import hashlib
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def analyze(path):
    raw = path.read_bytes()
    lines = raw.decode().splitlines()
    header = re.fullmatch(
        r'# fence_trace_v1 CPU=CLOCK_UPTIME_RAW_ns; cutoff=(\d+) first=(\d+) last=(\d+) records=(\d+) dropped=(\d+); cutoff is NOT completion, missing joins remain unknown',
        lines[0],
    )
    if not header:
        raise ValueError('Unrecognized trace header')
    cutoff, first, last, count, dropped = map(int, header.groups())
    records = []
    columns = 'kind cpu_ns frame thread sync fence latest done site value'.split()
    reader = csv.DictReader(lines[1:], delimiter='\t')
    if reader.fieldnames != columns:
        raise ValueError('Unexpected trace columns')
    for row in reader:
        if set(row) != set(columns) or any(v is None for v in row.values()):
            raise ValueError('Incomplete trace row')
        event = {k: int(v) for k, v in row.items()}
        if event['kind'] not in range(1, 8) or any(not math.isfinite(v) or v < 0 for v in event.values()):
            raise ValueError('Invalid event value')
        records.append(event)
    if len(records) != count:
        raise ValueError('Header count does not match complete rows')
    by_sync = defaultdict(list)
    for event in records:
        if event['sync']:
            by_sync[event['sync']].append(event)

    def unique(events, kind):
        found = [e for e in events if e['kind'] == kind]
        return found[0] if len(found) == 1 else None

    waits = []
    for identity, events in sorted(by_sync.items()):
        events.sort(key=lambda e: e['cpu_ns'])
        begin, end = unique(events, 1), unique(events, 2)
        issues = []
        if begin and end:
            if end['cpu_ns'] < begin['cpu_ns']:
                issues.append('end_before_begin')
            if end['fence'] != begin['fence'] or end['thread'] != begin['thread']:
                issues.append('endpoint_identity_mismatch')
        item = {'sync': identity, 'complete_pair': bool(begin and end and not issues),
                'endpoint_issues': issues, 'events': events}
        if begin:
            item.update(fence=begin['fence'], start_frame=begin['frame'], sync_type=begin['site'], requested_ns=begin['value'])
        if begin is not None and end is not None and not issues:
            item['duration_cpu_ms'] = (end['cpu_ns'] - begin['cpu_ns']) / 1e6
            item['end_frame'] = end['frame']
            item['result'] = end['value']
        waits.append(item)
    return {
        'input_sha256': hashlib.sha256(raw).hexdigest(), 'records': count, 'dropped': dropped,
        'first_frame': first, 'last_frame': last, 'cutoff_cpu_ns_upper_bound': cutoff,
        'limits': ['Capture cutoff is not completion; absent joins are unknown.',
                   'Only the wait side is traced: no submission, commit or GPU timing is joined, and no causal attribution is made.',
                   'Trace lock and export overhead can perturb gameplay; compare against an untraced run.',
                   'A complete endpoint pair does not prove that intermediate events were retained.'],
        'waits': waits,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.trace)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(f"{result['records']} records, {result['dropped']} dropped, {len(result['waits'])} guest waits")
