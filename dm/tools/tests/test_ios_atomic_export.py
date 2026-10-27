"""Legacy image atomics must be guarded before evaluating their device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "ios/tools/export_legacy_atomics.py"
SPEC = importlib.util.spec_from_file_location("atomic_export", TOOL)
assert SPEC is not None and SPEC.loader is not None
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)
SOURCE = """#define spvImage2DAtomicCoord(tc, tex) ((tex.get_width() * (tc).y) + (tc).x)
uint v = atomic_fetch_max_explicit((device atomic_uint*)&p[spvImage2DAtomicCoord(int3(f(x, y), y, layer), image)], uint(value), memory_order_relaxed);
uint unrelated = atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
"""


class AtomicExportTests(unittest.TestCase):
    def test_guards_nested_coordinates_and_preserves_unrelated_atomics(self):
        result, count = EXPORT.transform(SOURCE)
        self.assertEqual(count, 1)
        self.assertIn("(dm_image_atomic_valid(int3(f(x, y), y, layer), image) ? (atomic_fetch_max_explicit", result)
        self.assertIn("memory_order_relaxed)) : 0)", result)
        self.assertIn("atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed)", result)
        self.assertIn("uint(c.z) < t.get_array_size()", result)
        self.assertIn("ulong(uint(c.z)) * t.get_height()", result)

    def test_rejects_unguarded_or_malformed_coordinate_uses(self):
        for bad in (SOURCE + "uint k = spvImage2DAtomicCoord(c, image);\n",
                    SOURCE.replace("int3(f(x, y), y, layer), image", "c, image, extra"),
                    SOURCE.replace("#define spvImage2DAtomicCoord", "#define unrelated")):
            with self.subTest(source=bad), self.assertRaises(ValueError):
                EXPORT.transform(bad)

    def test_manifest_binds_variant_to_original_source_library_and_chain(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache, libs, out = root / "cache/b-abc", root / "ios/b-abc", root / "export"
            cache.mkdir(parents=True); libs.mkdir(parents=True)
            (cache / "a_b.metal").write_text(SOURCE)
            (libs / "a_b.metallib").write_bytes(b"MTLB-original")
            (libs / "a_b.json").write_text(json.dumps({"chain": "abc", "key": "a_b", "bindings": [{"msl_name": "image_atomic"}]}))
            self.assertEqual(EXPORT.export(cache, libs, out, "abc"), 1)
            record = json.loads((out / "a_b.json").read_text())
            self.assertEqual(record["original_source_sha256"], hashlib.sha256(SOURCE.encode()).hexdigest())
            self.assertEqual(record["source_sha256"], hashlib.sha256((out / "a_b.metal").read_bytes()).hexdigest())
            self.assertEqual(record["library_sha256"], hashlib.sha256(b"MTLB-original").hexdigest())
            with self.assertRaises(ValueError):
                EXPORT.export(cache, libs, out, "def")


if __name__ == "__main__":
    unittest.main()
