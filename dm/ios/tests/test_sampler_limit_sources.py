#!/usr/bin/env python3
"""Regression tests for exact unused-sampler companions."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools/export_sampler_limit_sources.py"
SPEC = importlib.util.spec_from_file_location("sampler_limit", TOOL)
SAMPLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SAMPLER)


def fixture():
    declarations = []
    bindings = []
    samples = []
    aid = 0
    for index in range(16):
        texture = f"texture_{index}"
        sampler = f"sampler_{index}"
        declarations += [f"texture2d<float> {texture} [[id({aid})]];", f"sampler {sampler} [[id({aid + 1})]];"]
        bindings += [
            {"msl_name": texture, "msl_type": "texture2d<float>", "arg_id": aid, "kind": "tex", "reg": index},
            {"msl_name": sampler, "msl_type": "sampler", "arg_id": aid + 1, "kind": "samp_tex", "reg": index},
        ]
        samples.append(f"color += args.{texture}.sample(args.{sampler}, float2(0.5));")
        aid += 2
    declarations += ["texture2d_array<float> read_only [[id(32)]];", "sampler unused_read_sampler [[id(33)]];"]
    bindings += [
        {"msl_name": "read_only", "msl_type": "texture2d_array<float>", "arg_id": 32, "kind": "tex", "reg": 24},
        {"msl_name": "unused_read_sampler", "msl_type": "sampler", "arg_id": 33, "kind": "samp_tex", "reg": 24},
    ]
    source = """#pragma clang diagnostic ignored \"-Wmissing-prototypes\"
#include <metal_stdlib>
using namespace metal;
struct spvDescriptorSetBuffer0 {
%s
};
fragment float4 main0(constant spvDescriptorSetBuffer0& args [[buffer(0)]]) {
    float4 color = float4(0.0);
    %s
    color += args.read_only.read(uint2(0), 0);
    return color;
}
""" % ("\n".join(declarations), "\n    ".join(samples))
    meta = {"key": "1111111111111111_b", "chain": "test", "route": "B", "stage": "ps", "entry": "main0", "bindings": bindings}
    return source, meta


class SamplerLimitProof(unittest.TestCase):
    def test_removes_only_globally_unused_scalar_sampler(self):
        source, meta = fixture()
        result, proof = SAMPLER.lower(source, meta)
        self.assertEqual(proof["original_sampler_count"], 17)
        self.assertEqual(proof["remaining_sampler_count"], 16)
        self.assertEqual(proof["removed"], [{"msl_name": "unused_read_sampler", "msl_type": "sampler", "arg_id": 33,
                                              "kind": "samp_tex", "reg": 24}])
        self.assertNotIn("sampler unused_read_sampler [[id(33)]];", result)
        self.assertIn("args.read_only.read(uint2(0), 0)", result)
        for index in range(16):
            self.assertIn(f"sampler sampler_{index} [[id({index * 2 + 1})]];", result)
            self.assertIn(f"args.texture_{index}.sample(args.sampler_{index}", result)

    def test_identifier_in_any_helper_or_entry_is_not_unused(self):
        source, meta = fixture()
        source += "\nfloat helper(constant spvDescriptorSetBuffer0& args) { return args.unused_read_sampler.get_lod_min_clamp(); }\n"
        with self.assertRaisesRegex(ValueError, "Not enough statically unused"):
            SAMPLER.lower(source, meta)

    def test_all_seventeen_sampled_is_rejected(self):
        source, meta = fixture()
        source = source.replace("args.read_only.read(uint2(0), 0)", "args.read_only.sample(args.unused_read_sampler, float2(0.0), 0)")
        with self.assertRaisesRegex(ValueError, "Not enough statically unused"):
            SAMPLER.lower(source, meta)

    def test_reflection_or_declaration_disagreement_is_rejected(self):
        source, meta = fixture()
        changes = []
        changed = copy.deepcopy(meta)
        changed["bindings"][-1]["arg_id"] = 34
        changes.append((source, changed, "argument ID mismatch"))
        changed = copy.deepcopy(meta)
        changed["bindings"][-1]["msl_name"] = "other_sampler"
        changes.append((source, changed, "declarations and reflection"))
        changed = copy.deepcopy(meta)
        changed["bindings"][-1]["kind"] = "tex"
        changes.append((source, changed, "kind mismatch"))
        changed = copy.deepcopy(meta)
        changed["bindings"][-1]["arg_id"] = changed["bindings"][-2]["arg_id"]
        changes.append((source, changed, "Duplicate resource"))
        for candidate_source, candidate_meta, reason in changes:
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(ValueError, reason):
                    SAMPLER.lower(candidate_source, candidate_meta)

    def test_sampler_array_and_preprocessor_rejected(self):
        source, meta = fixture()
        array_meta = copy.deepcopy(meta)
        array_meta["bindings"][-1]["msl_type"] = "array<sampler, 2>"
        with self.assertRaisesRegex(ValueError, "Sampler arrays"):
            SAMPLER.lower(source, array_meta)
        for directive in ("#define unused_read_sampler hidden\n", "#if 1\n#endif\n"):
            with self.subTest(directive=directive):
                with self.assertRaisesRegex(ValueError, "Unsupported preprocessor"):
                    SAMPLER.lower(directive + source, meta)

    def test_comments_and_strings_do_not_make_sampler_live(self):
        source, meta = fixture()
        source = source.replace("float4 color", "// unused_read_sampler\n    float4 color")
        result, proof = SAMPLER.lower(source, meta)
        self.assertNotIn("sampler unused_read_sampler", result)
        self.assertEqual(proof["remaining_sampler_count"], 16)

    def test_export_binds_source_library_and_reflection_hashes(self):
        source, meta = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache/b-test"
            libraries = root / "libraries/b-test"
            output = root / "output/sampler-limit-v1"
            cache.mkdir(parents=True)
            libraries.mkdir(parents=True)
            key = meta["key"]
            source_bytes = source.encode()
            reflection_bytes = json.dumps(meta, separators=(",", ":")).encode()
            library_bytes = b"synthetic base Metal library"
            (cache / (key + ".metal")).write_bytes(source_bytes)
            (libraries / (key + ".json")).write_bytes(reflection_bytes)
            (libraries / (key + ".metallib")).write_bytes(library_bytes)

            records = SAMPLER.export(cache, libraries, output, "test")
            record = records[key]
            self.assertTrue(record["supported"])
            self.assertEqual(record["source"], key + ".metal")
            for field, data in (("original_source_sha256", source_bytes), ("reflection_sha256", reflection_bytes),
                                ("base_library_sha256", library_bytes),
                                ("source_sha256", (output / record["source"]).read_bytes())):
                self.assertEqual(record[field], hashlib.sha256(data).hexdigest())
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["kind"], "sampler-limit-v1")
            self.assertEqual(manifest["limit"], 16)
            self.assertEqual((libraries / (key + ".metallib")).read_bytes(), library_bytes)

            first = (output / record["source"]).read_bytes()
            self.assertEqual(SAMPLER.export(cache, libraries, output, "test")[key]["source_sha256"], hashlib.sha256(first).hexdigest())
            self.assertEqual((output / record["source"]).read_bytes(), first)

    def test_unproven_export_is_recorded_but_not_emitted(self):
        source, meta = fixture()
        source = source.replace("args.read_only.read(uint2(0), 0)", "args.read_only.sample(args.unused_read_sampler, float2(0.0), 0)")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "b-test"
            libraries = root / "libraries/b-test"
            output = root / "sampler-limit-v1"
            cache.mkdir(parents=True)
            libraries.mkdir(parents=True)
            key = meta["key"]
            (cache / (key + ".metal")).write_text(source)
            (libraries / (key + ".json")).write_text(json.dumps(meta))
            (libraries / (key + ".metallib")).write_bytes(b"base")
            record = SAMPLER.export(cache, libraries, output, "test")[key]
            self.assertFalse(record["supported"])
            self.assertIn("Not enough statically unused", record["reason"])
            self.assertFalse((output / (key + ".metal")).exists())


if __name__ == "__main__":
    unittest.main()
