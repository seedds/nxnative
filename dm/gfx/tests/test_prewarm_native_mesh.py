"""Source associations and output compatibility for prospective mesh prewarming."""
import importlib.util
from pathlib import Path
import unittest

FILE = Path(__file__).resolve().parents[3] / 'scripts/prewarm_native_mesh.py'
SPEC = importlib.util.spec_from_file_location('prewarm_native_mesh', FILE)
assert SPEC and SPEC.loader
seed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(seed)


class MeshSeedTests(unittest.TestCase):
    def test_programs_do_not_cross_variations(self):
        found = [('model#BNSH@0#var0', st, key, b'') for st, key in [('ts', 'task0'), ('ms', 'mesh0'), ('ps', 'pixel0')]]
        found += [('model#BNSH@0#var1', st, key, b'') for st, key in [('ms', 'mesh1'), ('ps', 'pixel1')]]
        found += [('other#BNSH@0#var0', 'vs', 'vertex', b''), ('raw#GLSLC', 'ms', 'ignored', b'')]
        self.assertEqual(seed.mesh_programs(found, lambda control, code: control),
                         {('task0', 'mesh0', 'pixel0'), ('-', 'mesh1', 'pixel1')})

    def test_container_source_is_a_physical_file(self):
        self.assertEqual(seed.source_file('/Shader/app.sarc.zs/Model.bfsha#BNSH@0#var0'), '/Shader/app.sarc.zs')
        self.assertEqual(seed.source_file('/Shader/model.bfsha.zs#BNSH@0#var1'), '/Shader/model.bfsha.zs')

    def test_output_signature_is_explicit(self):
        self.assertEqual(seed.pixel_signature({'raster': {'colors': [0, 1], 'omap': 255}}), ((0, 1), 255))
        with self.assertRaises(ValueError):
            seed.pixel_signature({'raster': {'colors': [0]}})

    def fixtures(self):
        return {
            'mesh_b': {'chain': 'chain', 'mesh_stage': {'kind': 'ms', 'task': True, 'payload': 256}},
            'task_b': {'chain': 'chain', 'mesh_stage': {'kind': 'ts', 'payload': 256}},
            'pixel_b': {'chain': 'chain', 'stage': 'ps', 'raster': {'colors': [0], 'omap': 15}},
        }

    def test_stage_payload_and_chain_validation(self):
        for part, field, bad in [('task_b', 'chain', 'old'), ('mesh_b', 'chain', 'old'), ('pixel_b', 'stage', 'cs')]:
            with self.subTest(part=part, field=field):
                data = self.fixtures()
                data[part][field] = bad
                with self.assertRaises(ValueError):
                    seed.validate_program(('task', 'mesh', 'pixel'), data.__getitem__, 'chain')
        data = self.fixtures()
        self.assertEqual(seed.validate_program(('task', 'mesh', 'pixel'), data.__getitem__, 'chain'), ((0,), 15))
        data['task_b']['mesh_stage']['payload'] = 128
        with self.assertRaises(ValueError):
            seed.validate_program(('task', 'mesh', 'pixel'), data.__getitem__, 'chain')

    def test_taskless_program_must_have_taskless_reflection(self):
        data = self.fixtures()
        with self.assertRaises(ValueError):
            seed.validate_program(('-', 'mesh', 'pixel'), data.__getitem__, 'chain')
        data['mesh_b']['mesh_stage']['task'] = False
        self.assertEqual(seed.validate_program(('-', 'mesh', 'pixel'), data.__getitem__, 'chain'), ((0,), 15))

    def test_malformed_template_headers_are_rejected(self):
        data = bytearray(52)
        data[24:31] = b'pixel_b'
        data[48:50] = bytes([4, 1])
        row = lambda value: 'M\t-\t' + value.hex()
        self.assertEqual(seed.seed_pixel(row(data)), 'pixel_b')
        for offset, value in [(48, 0), (49, 5), (50, 1), (51, 1)]:
            bad = data.copy()
            bad[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                seed.seed_pixel(row(bad))
        for text in ['M\t-\txx', 'M\t-\t00', 'R\t00']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                seed.seed_pixel(text)


if __name__ == '__main__':
    unittest.main()
