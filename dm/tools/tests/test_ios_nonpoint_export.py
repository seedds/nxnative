"""Supplemental shader exports must preserve data and reject mixed identities."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "ios/tools/export_nonpoint_sources.py"
SPEC = importlib.util.spec_from_file_location("nonpoint_export", TOOL)
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class NonPointExportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.cache, self.libs, self.out = root / "cache/b-abc", root / "ios/b-abc", root / "export"
        self.cache.mkdir(parents=True)
        self.libs.mkdir(parents=True)
        self.source = b"float gl_PointSize [[point_size]];\n"
        (self.cache / "a_b.metal").write_bytes(self.source)
        (self.libs / "a_b.metallib").write_bytes(b"MTLB-original")
        self.reflection = self.libs / "a_b.json"
        self.meta = {"stage": "vs", "key": "a_b", "chain": "abc"}
        self.reflection.write_text(json.dumps(self.meta))

    def test_export_preserves_source_and_binds_both_hashes(self):
        self.assertEqual(EXPORT.export(self.cache, self.libs, self.out, "abc"), 1)
        manifest = json.loads((self.out / "manifest.json").read_text())
        self.assertEqual(manifest["chain"], "abc")
        self.assertEqual((self.out / "a_b.metal").read_bytes(), self.source)
        self.assertEqual(manifest["shaders"]["a_b"], {
            "source_sha256": hashlib.sha256(self.source).hexdigest(),
            "library_sha256": hashlib.sha256(b"MTLB-original").hexdigest(),
        })

    def test_rejects_wrong_chain_directory_and_reflection(self):
        with self.assertRaises(ValueError):
            EXPORT.export(self.cache, self.libs, self.out, "def")
        self.meta["chain"] = "def"
        self.reflection.write_text(json.dumps(self.meta))
        with self.assertRaises(ValueError):
            EXPORT.export(self.cache, self.libs, self.out, "abc")
        self.assertFalse((self.out / "manifest.json").exists())

    def test_does_not_export_fragment_or_nonpoint_free_source(self):
        self.meta["stage"] = "ps"
        self.reflection.write_text(json.dumps(self.meta))
        self.assertEqual(EXPORT.export(self.cache, self.libs, self.out, "abc"), 0)
        self.meta["stage"] = "vs"
        self.reflection.write_text(json.dumps(self.meta))
        (self.cache / "a_b.metal").write_text("float4 position [[position]];\n")
        self.assertEqual(EXPORT.export(self.cache, self.libs, self.out, "abc"), 0)
        self.assertEqual(json.loads((self.out / "manifest.json").read_text())["shaders"], {})


if __name__ == "__main__":
    unittest.main()
