"""Analyze optional audren PCM provenance; never plays audio or starts the game."""
import argparse
from array import array
import hashlib
import math
import sys
from collections import Counter, defaultdict, deque
import csv
import json
from pathlib import Path


def pcm_hash(data):
    h = 14695981039346656037
    for byte in data: h = ((h ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{h:016x}'


def pcm_summary(path, rate=48000, channels=2, block_frames=240):
    data = Path(path).read_bytes(); aligned = len(data) - len(data) % (channels * 2)
    samples = array('h'); samples.frombytes(data[:aligned])
    if sys.byteorder != 'little': samples.byteswap()
    if not samples: return {'frames': 0, 'trailing_bytes': len(data)}
    width = block_frames * channels * 2; history = deque(maxlen=2)
    repeats = run = longest = zero = 0; longest_end = 0
    for i, off in enumerate(range(0, aligned - width + 1, width)):
        block = memoryview(data)[off:off + width]; nonzero = any(block)
        key = hashlib.blake2b(block, digest_size=12).digest() if nonzero else None
        if key and len(history) == 2 and history[0] == key:
            repeats += 1; run += 1
            if run > longest: longest = run; longest_end = i + 1
        else: run = 0
        if not nonzero: zero += 1
        history.append(key)
    frames = len(samples) // channels
    return {'frames': frames, 'seconds': frames / rate, 'sample_min': min(samples), 'sample_max': max(samples),
            'rms': math.sqrt(sum(v * v for v in samples) / len(samples)),
            'saturated_samples': samples.count(-32768) + samples.count(32767),
            'zero_blocks': zero, 'nonzero_lag2_repeat_blocks': repeats,
            'max_consecutive_lag2_repeat_blocks': longest,
            'longest_repeat_start_s': (longest_end - longest) * block_frames / rate if longest else None,
            'longest_repeat_end_s': longest_end * block_frames / rate if longest else None,
            'trailing_bytes': len(data) - aligned,
            'format': f'PCM16 little-endian, {channels} channels at {rate} Hz; analysis blocks {block_frames} frames'}


def analyze(path, pcm=None):
    with Path(path).open() as f:
        rows = list(csv.DictReader((s for s in f if not s.startswith('#')), delimiter='\t'))
    counts = Counter(); examples = []; sources = {}; prev = {}; histories = defaultdict(lambda: deque(maxlen=2))
    renderers = {r['renderer'] for r in rows if r['stage'].startswith('submit-')}
    raw = Path(pcm).open('rb') if pcm and len(renderers) == 1 else None
    try:
        for row in rows:
            stage, renderer = row['stage'], row['renderer']; seq, n = int(row['sequence']), int(row['samples'])
            key = renderer, seq; signal = n, row['hash']
            phase = 'source' if stage.startswith('source-') else 'submit' if stage.startswith('submit-') else None
            if not phase: continue
            counts[phase + '_frames'] += 1
            history = histories[phase, renderer]
            if n and int(row['nonzero']) and len(history) == 2 and history[0] == signal:
                counts[phase + '_nonzero_lag2_repeats'] += 1
            history.append(signal)
            if phase == 'source':
                if key in sources: counts['duplicate_source_ids'] += 1
                sources[key] = row
                if int(row['remaining']) > 0:
                    counts['incomplete_processor_frames'] += 1
                    if len(examples) < 20: examples.append({'kind': 'processor_remaining', **row})
                continue
            if not int(row['accepted']): counts['rejected_appends'] += 1
            if renderer in prev and seq <= prev[renderer]:
                counts['nonincreasing_submission_ids'] += 1
                if len(examples) < 20: examples.append({'kind': 'submission_order', **row})
            prev[renderer] = seq
            source = sources.get(key)
            if source is None: counts['unmatched_submissions'] += 1
            elif int(source['samples']) == 0 and not int(row['nonzero']): counts['silent_fallbacks'] += 1
            elif signal != (int(source['samples']), source['hash']):
                counts['source_copy_mismatches'] += 1
                if len(examples) < 20: examples.append({'kind': 'source_copy', **row})
            if raw:
                data = raw.read(n * 4)
                if len(data) != n * 4: counts['raw_truncated_frames'] += 1
                elif pcm_hash(data) != row['hash']: counts['raw_submission_mismatches'] += 1
        if raw: counts['raw_unmatched_tail_bytes'] = len(raw.read())
    finally:
        if raw: raw.close()
    return {'counts': dict(counts), 'raw_checked': pcm is not None and len(renderers) == 1,
            'renderers': sorted(renderers), 'examples': examples,
            'limits': 'Recorded sample repeats alone do not prove a bug; missing IDs and incomplete capture tails are unknown, not evidence of corruption. Raw comparison assumes this renderer is the only AudioOut captured; multiple outputs require separate recordings.'}


def aux_summary(path):
    """Observe API transfers, not guessed ring fill levels or synthetic acknowledgements."""
    rings = {}; written = {}; history = {}; counts = Counter()
    with Path(path).open() as f:
        for row in csv.DictReader((s for s in f if not s.startswith('#')), delimiter='\t'):
            op = row['operation']; counts[op] += 1
            key = row['info'], row['buffer']
            ring = rings.setdefault(key, {'info': key[0], 'buffer': key[1], 'operations': Counter(), 'capacities': set(),
                                         'short_transfers': 0, 'dsp_lag2_repeats': 0, 'dsp_chunks_seen_in_cpu_writes': 0})
            ring['operations'][op] += 1; ring['capacities'].add(int(row['capacity']))
            requested, got = int(row['requested']), int(row['transferred'])
            if got >= 0 and got < requested: ring['short_transfers'] += 1
            if op == 'dsp-write' and got < 0: counts['void_dsp_writes_not_acknowledged'] += 1
            blocks = [h for h in row['blocks'].split(',') if h]
            if op == 'cpu-write':
                for h in blocks: written.setdefault((key, h), int(row['end_ns']))
            if op == 'dsp-read':
                h = row['hash']; lane = key, row['offset']; hist = history.setdefault(lane, deque(maxlen=2))
                if row.get('nonzero') == '1' and len(hist) == 2 and hist[0] == h: ring['dsp_lag2_repeats'] += 1
                hist.append(h)
                ring['dsp_chunks_seen_in_cpu_writes'] += sum(written.get((key, h), sys.maxsize) <= int(row['start_ns']) for h in blocks)
    for ring in rings.values():
        ring['operations'] = dict(ring['operations']); ring['capacities'] = sorted(ring['capacities'])
    return {'counts': dict(counts), 'rings': list(rings.values()),
            'limits': 'Opaque SDK ring state; resets, overlapping operations, unhooked/grouped/inlined calls and diagnostic I/O may affect interpretation. A repeated hash is not by itself an underflow. DSP writes return void; no write success is inferred. CPU write chunk matches require an earlier completed call.'}


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('trace', type=Path, nargs='?'); p.add_argument('--pcm', type=Path)
    a = p.parse_args()
    if not a.trace and not a.pcm: p.error('give a trace, --pcm, or both')
    result = analyze(a.trace, a.pcm) if a.trace else {}
    if a.pcm: result['pcm'] = pcm_summary(a.pcm)
    if a.trace and (aux := Path(str(a.trace) + '.aux.tsv')).is_file(): result['aux'] = aux_summary(aux)
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
