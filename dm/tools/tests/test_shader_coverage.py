"""The strict full-library gate must reject incomplete builds that otherwise finish."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import shader_coverage


class Coverage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.lib = self.root / 'b-test'; self.lib.mkdir()
        self.key = 'a' * 16
        self.g = types.SimpleNamespace(id='G', rows=[], table=lambda name: self.root / name)
        (self.root / 'shader_map.tsv').write_text(f'p.bin\t0\tpixel\t{self.key}\tsrc\n')
        self.stamp()
        self.refl = {'dm_ctl': self.key, 'chain': 'test', 'stage': 'ps', 'bindings': []}
        self.rows = [{'key': self.key, 'st': 'ok', 'err': ''}]
        self.save()
        (self.lib / (self.key + '_b.metallib')).write_bytes(b'fixture')

    def stamp(self):
        digest = shader_coverage.dmsh_chain.inputs(self.g)
        (self.lib / 'inputs.tsv').write_text(f'game\tG\ninputs\t{digest}\n')
        for name in ('c1_full.tsv', 'cb_extent.tsv'):
            (self.root / name).write_text(f'# chain b-test inputs {digest} columns follow\n')

    def save(self):
        (self.lib / '_b_build.json').write_text(json.dumps(self.rows))
        (self.lib / (self.key + '_b.json')).write_text(json.dumps(self.refl))

    def check(self):
        return shader_coverage.audit(self.g, self.lib)

    def test_complete(self):
        self.assertTrue(self.check()['complete'])

    def test_failed_or_missing_report_row(self):
        for rows in ([], [{'key': self.key, 'st': 'compile failed', 'err': 'bad MSL'}]):
            self.rows = rows; self.save(); self.assertFalse(self.check()['complete'])

    def test_missing_library_and_reflection(self):
        for ext in ('.metallib', '.json'):
            p = self.lib / (self.key + '_b' + ext); data = p.read_bytes(); p.unlink()
            self.assertFalse(self.check()['complete']); p.write_bytes(data)

    def test_stamp_and_table_mismatch(self):
        for name in ('inputs.tsv',):
            p = self.lib / name; data = p.read_text(); p.write_text('game\tWRONG\ninputs\tI\n')
            self.assertFalse(self.check()['complete']); p.write_text(data)
        (self.root / 'cb_extent.tsv').write_text('# wrong stamp\n')
        self.assertFalse(self.check()['complete'])

    def test_mandatory_dual_and_variant(self):
        (self.root / 'dual_want.tsv').write_text(self.key + '\n'); self.stamp()
        self.assertFalse(self.check()['complete'])
        self.refl['entry_d'] = 'main0_d'; self.save(); self.assertTrue(self.check()['complete'])
        self.refl['pull'] = {'entry': 'main0'}; self.save()
        self.assertFalse(self.check()['complete'])

    def test_silent_losses_unresolved_bindings_and_driver_reads(self):
        for fields in ({'entries_lost': ['dual']}, {'notes': ['translator: Failed to find handle source for bindless access of type "x"']},
                       {'drv_unfillable': ['0:4']}, {'bindings': [{'kind': 'tex', 'reg': None}]}, {'bindings': [{'kind': 'unknown'}]}):
            self.refl = {'dm_ctl': self.key, 'chain': 'test', 'stage': 'ps', **fields}; self.save()
            self.assertFalse(self.check()['complete'], fields)

    def test_geometry_requires_all_topologies(self):
        (self.root / 'shader_map.tsv').write_text(f'p.bin\t0\tgeometry\t{self.key}\tsrc\n')
        self.stamp(); self.refl['stage'] = 'gs'; self.save(); self.assertFalse(self.check()['complete'])
        for suffix in ['_bg' + str(i) for i in range(5)] + ['_bgv']:
            for ext in ('.json', '.metallib'): (self.lib / (self.key + suffix + ext)).write_bytes(b'{}' if ext == '.json' else b'fixture')
        self.assertFalse(self.check()['complete'])
        for suffix in ['_bg' + str(i) for i in range(5)] + ['_bgv']:
            (self.lib / (self.key + suffix + '.json')).write_text(json.dumps(self.refl))
        self.assertTrue(self.check()['complete'])

    def test_empty_files_and_invalid_report(self):
        for value in ('{}', 'null', '[]', 'bad json'):
            (self.lib / (self.key + '_b.json')).write_text(value)
            self.assertFalse(self.check()['complete'])
        self.save()
        (self.lib / (self.key + '_b.metallib')).write_bytes(b'')
        self.assertFalse(self.check()['complete'])
        for value in ('{}', '[{}]', '[null]'):
            (self.lib / '_b_build.json').write_text(value)
            self.assertFalse(self.check()['complete'])

    def test_current_profile_digest_and_extra_report_keys(self):
        (self.root / 'dual_want.tsv').write_text(self.key + '\n')
        self.assertIn('missing or incompatible full-library stamp', self.check()['issues'])
        self.rows += [{'key': 'b' * 16, 'st': 'ok'}]; self.save()
        self.assertIn('unexpected shader keys in full-build report', self.check()['issues'])

    def test_packed_library(self):
        shader_coverage.shader_lib.pack(self.lib)
        self.assertFalse(self.lib.exists())
        self.assertTrue(self.check()['complete'])

    def test_layer_only_geometry_needs_reflection_without_a_library(self):
        (self.root / 'shader_map.tsv').write_text(f'p.bin\t0\tgeometry\t{self.key}\tsrc\n')
        self.stamp(); self.refl.update(stage='gs', gs_slice={'loc': 3, 'comp': 'x'}); self.save()
        (self.lib / (self.key + '_b.metallib')).unlink()
        self.assertTrue(self.check()['complete'])
        self.refl.pop('gs_slice'); self.save()
        self.assertFalse(self.check()['complete'])

    def test_ordinary_side_reflection_is_embedded_in_base(self):
        for field, suffix in (('pull', '_bp'), ('vin', '_bv'), ('fetch', '_bf'), ('alpha', '_bx'), ('logic', '_bx')):
            self.refl[field] = {'entry': 'main0'}; self.save()
            p = self.lib / (self.key + suffix + '.metallib')
            self.assertFalse(self.check()['complete'])
            p.write_bytes(b'fixture')
            self.assertTrue(self.check()['complete'])
            p.unlink(); self.refl.pop(field)

    def test_geometry_variant_translation_loss(self):
        self.test_geometry_requires_all_topologies()
        p = self.lib / (self.key + '_bg1.json')
        p.write_text(json.dumps({**self.refl, 'entries_lost': ['layer']}))
        self.assertFalse(self.check()['complete'])

    def test_identity_and_stage(self):
        for field, value in (('chain', 'wrong'), ('dm_ctl', 'wrong'), ('stage', 'vs')):
            old = self.refl[field]; self.refl[field] = value; self.save()
            self.assertFalse(self.check()['complete']); self.refl[field] = old


if __name__ == '__main__':
    unittest.main()
