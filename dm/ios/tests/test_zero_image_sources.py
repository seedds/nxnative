#!/usr/bin/env python3
"""Proof rejection and hash-binding tests using synthetic Metal source."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools/export_zero_image_sources.py"
SPEC = importlib.util.spec_from_file_location("zero_image", TOOL)
ZERO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ZERO)

SOURCE = """#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
struct _10 {
uint _m0; uint4 _m1[8]; float4 _m2; float4 _m3; int _m4;
float4 _m5[73]; int4 _m6; int _m7;
};
struct Arguments {
constant _10* support [[id(0)]];
texture2d_array<float, access::write> target [[id(1)]];
};
fragment void clear_test(constant Arguments& args [[buffer(0)]], float4 pos [[position]]) {
float frag_x = pos.x;
float frag_y = pos.y;
float scale = (*args.support)._m5[0].x;
float scaled_x = frag_x / scale;
float scaled_y = frag_y / scale;
float rounded_x = trunc(scaled_x);
float rounded_y = trunc(scaled_y);
int coord_x = int(rounded_x);
int coord_y = int(rounded_y);
int3 pixel0 = int3(coord_x, coord_y, 0);
args.target.write(float4(0.0), uint2(pixel0.xy), uint(pixel0.z));
int3 pixel2 = int3(coord_x, coord_y, 2);
args.target.write(float4(0.0), uint2(pixel2.xy), uint(pixel2.z));
}
"""
META = {"key": "1111111111111111_b", "chain": "test", "stage": "ps", "entry": "clear_test", "msl_entry_kind": "fragment",
        "raster": {"discard": 0, "sample_mask": 0, "colors": []}, "vertex_inputs": [], "drv_reads": {}, "drv_unfillable": [],
        "bindings": [{"kind": "support", "msl_name": "support", "msl_type": "constant _10*", "arg_id": 0},
                     {"kind": "img", "msl_name": "target", "msl_type": "texture2d_array<float, access::write>", "arg_id": 1, "reg": 1}]}


class ZeroImageProof(unittest.TestCase):
    def test_preserves_fragment_coordinates_and_atomic_word_zeroes(self):
        result, proof = ZERO.lower(SOURCE, META)
        self.assertEqual(proof["layers"], [0, 2])
        self.assertEqual(proof["parameter_slot"], 29)
        self.assertEqual(proof["supported_pixel_bytes"], [4, 8, 16])
        self.assertIn("device atomic_uint* target [[id(1)]]", result)
        self.assertIn("constant uint* dm_zero_layout [[buffer(29)]]", result)
        self.assertIn("float scaled_x = frag_x / scale;", result)
        self.assertEqual(result.count("dm_zero_store(args.target,"), 2)
        self.assertNotIn(".write(", result)
        self.assertIn("atomic_store_explicit(image + at + word, 0u, memory_order_relaxed)", result)
        self.assertIn("ulong(layer) * image_words", result)

    def test_rejects_unproven_source_changes(self):
        changes = [
            ("float4(0.0)", "float4(1.0)"),
            ("trunc(scaled_x)", "unknown_function(scaled_x)"),
            ("float frag_x = pos.x;", "float frag_x = args.target.read(uint2(0)).x;"),
            ("float frag_x = pos.x;", "discard_fragment(); float frag_x = pos.x;"),
            ("float frag_x = pos.x;", "atomic_store_explicit(hidden, 1u, memory_order_relaxed); float frag_x = pos.x;"),
            ("float frag_x = pos.x;", "float frag_x = pos.x; frag_x += 1.0;"),
            ("float frag_x = pos.x;", "if (pos.x > 0) { return; } float frag_x = pos.x;"),
            ("float frag_x = pos.x;", "float frag_x = pos.x; return;"),
            ("._m5[0].x", "._m5[0].y"),
            ("int3(coord_x, coord_y, 2)", "int3(coord_y, coord_x, 2)"),
            ("int3(coord_x, coord_y, 2)", "int3(coord_x, coord_y, -1)"),
            ("[[buffer(0)]]", "[[buffer(29)]]"),
            ("float frag_x = pos.x;", "float dm_zero_layout = pos.x;"),
            ("int _m7;", "int _m7; device uint* hidden;"),
            ("using namespace metal;", "using namespace metal;\n#define write ignore"),
            ("fragment void clear_test", "float injected() { return 0; } fragment void clear_test"),
        ]
        for old, new in changes:
            with self.subTest(change=new):
                with self.assertRaises(ValueError):
                    ZERO.lower(SOURCE.replace(old, new, 1), META)

    def test_rejects_reflection_disagreement(self):
        for path, value in [(('raster', 'discard'), 1), (('raster', 'sample_mask'), 1),
                            (('raster', 'colors'), [0]), (('bindings', 1, 'kind'), 'imga'),
                            (('bindings', 1, 'msl_type'), 'texture2d_array<float, access::read_write>'),
                            (('bindings', 0, 'rw'), 1), (('entry',), 'other')]:
            meta = copy.deepcopy(META)
            cursor = meta
            for item in path[:-1]:
                cursor = cursor[item]
            cursor[path[-1]] = value
            with self.subTest(path=path):
                with self.assertRaises(ValueError):
                    ZERO.lower(SOURCE, meta)

    def test_export_hash_binds_base_reflection_and_both_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / "cache/b-test"
            libraries = root / "libraries/b-test"
            output = root / "output/zero-image-v1"
            cache.mkdir(parents=True)
            libraries.mkdir(parents=True)
            key = META['key']
            original = SOURCE.encode()
            reflection = json.dumps(META).encode()
            base = b'synthetic base library, never modified'
            (cache / (key + '.metal')).write_bytes(original)
            (libraries / (key + '.json')).write_bytes(reflection)
            (libraries / (key + '.metallib')).write_bytes(base)
            records = ZERO.export(cache, libraries, output, 'test')
            record = records[key]
            for field, data in [('original_source_sha256', original), ('reflection_sha256', reflection),
                                ('base_library_sha256', base), ('source_sha256', (output / record['source']).read_bytes())]:
                self.assertEqual(record[field], hashlib.sha256(data).hexdigest())
            self.assertEqual((libraries / (key + '.metallib')).read_bytes(), base)
            self.assertEqual(json.loads((output / 'manifest.json').read_text())['kind'], 'zero-image-clear-v1')
            (cache / (key + '.metal')).write_text(SOURCE.replace('float4(0.0)', 'float4(9.0)'))
            self.assertEqual(ZERO.export(cache, libraries, output, 'test'), {})


if __name__ == '__main__':
    unittest.main()
