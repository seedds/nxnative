"""Provenance analysis distinguishes repeated source PCM, copy corruption and playback order."""
import csv
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audio_trace


class AudioTrace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.trace = Path(self.tmp.name) / 'frames.tsv'; self.pcm = self.trace.with_suffix('.pcm'); self.rows = []

    def row(self, stage, seq, value, remaining=0):
        data = struct.pack('<hh', value, -value)
        return {'stage': stage, 'renderer': '0', 'sequence': str(seq), 'samples': '1', 'hash': audio_trace.pcm_hash(data),
                'nonzero': str(int(value != 0)), 'remaining': str(remaining), 'accepted': '1'}

    def run_rows(self):
        with self.trace.open('w') as f:
            f.write('# trace fixture\n'); w = csv.DictWriter(f, fieldnames=list(self.rows[0]), delimiter='\t'); w.writeheader(); w.writerows(self.rows)
        return audio_trace.analyze(self.trace, self.pcm)

    def test_known_hash_and_correct_stream(self):
        self.assertEqual(audio_trace.pcm_hash(b'hello'), 'a430d84680aabd0b')
        self.rows = [self.row(stage, i, i) for i in range(1, 4) for stage in ['source-render', 'submit-render']]
        self.pcm.write_bytes(b''.join(struct.pack('<hh', i, -i) for i in range(1, 4)))
        c = self.run_rows()['counts']
        self.assertEqual(c['source_frames'], 3); self.assertEqual(c['submit_frames'], 3)
        self.assertNotIn('source_copy_mismatches', c); self.assertNotIn('raw_submission_mismatches', c)

    def test_source_repetition_is_distinct_from_copy_corruption(self):
        values = [1, 2, 1, 2]
        self.rows = [self.row(stage, i, value) for i, value in enumerate(values, 1) for stage in ['source-render', 'submit-render']]
        self.pcm.write_bytes(b''.join(struct.pack('<hh', v, -v) for v in values))
        c = self.run_rows()['counts']
        self.assertEqual(c['source_nonzero_lag2_repeats'], 2)
        self.assertNotIn('source_copy_mismatches', c)
        self.rows[-1] = self.row('submit-render', 4, 9)
        c = self.run_rows()['counts']
        self.assertEqual(c['source_copy_mismatches'], 1); self.assertEqual(c['raw_submission_mismatches'], 1)

    def test_pcm_summary_counts_real_samples_and_repeats(self):
        self.pcm.write_bytes(b''.join(struct.pack('<hh', v, -v) for v in [1, 2, 1, 2]))
        report = audio_trace.pcm_summary(self.pcm, block_frames=1)
        self.assertEqual(report['frames'], 4); self.assertEqual(report['sample_min'], -2)
        self.assertEqual(report['saturated_samples'], 0); self.assertEqual(report['nonzero_lag2_repeat_blocks'], 2)
        self.pcm.write_bytes(struct.pack('<hh', 32767, -32768) + b'x')
        report = audio_trace.pcm_summary(self.pcm, block_frames=1)
        self.assertEqual(report['saturated_samples'], 2); self.assertEqual(report['trailing_bytes'], 1)

    def test_order_missing_source_incomplete_processing_and_truncated_capture(self):
        self.rows = [self.row('source-render', 1, 1, remaining=2), self.row('source-catchup', 2, 2),
                     self.row('submit-catchup', 2, 2), self.row('submit-render', 1, 1), self.row('submit-render', 9, 3)]
        self.pcm.write_bytes(struct.pack('<hh', 2, -2))
        c = self.run_rows()['counts']
        self.assertEqual(c['nonincreasing_submission_ids'], 1); self.assertEqual(c['unmatched_submissions'], 1)
        self.assertEqual(c['incomplete_processor_frames'], 1); self.assertEqual(c['raw_truncated_frames'], 2)

    def test_aux_short_io_void_writes_and_matching_chunks(self):
        path = Path(str(self.trace) + '.aux.tsv')
        rows = []
        def event(op, h, got=240, info='1', start=10):
            return {'operation': op, 'info': info, 'buffer': '2', 'capacity': '480', 'requested': '240',
                    'transferred': str(got), 'offset': '0', 'hash': h, 'nonzero': '1', 'blocks': h + ',', 'start_ns': str(start), 'end_ns': str(start + 1)}
        rows.append(event('cpu-write', 'a', start=1))
        rows += [event('dsp-read', h) for h in ['a', 'b', 'a']]
        rows.append(event('cpu-write', 'c', got=12))
        rows.append(event('dsp-write', 'd', got=-1))
        rows.append(event('dsp-read', 'a', info='other'))
        with path.open('w') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]), delimiter='\t'); w.writeheader(); w.writerows(rows)
        report = audio_trace.aux_summary(path); ring = report['rings'][0]
        self.assertEqual(ring['short_transfers'], 1); self.assertEqual(ring['dsp_lag2_repeats'], 1)
        self.assertEqual(ring['dsp_chunks_seen_in_cpu_writes'], 2)
        self.assertEqual(report['counts']['void_dsp_writes_not_acknowledged'], 1)
        self.assertEqual(report['rings'][1]['dsp_chunks_seen_in_cpu_writes'], 0)


if __name__ == '__main__': unittest.main()
