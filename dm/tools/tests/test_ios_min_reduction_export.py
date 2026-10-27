"""MIN companions are structurally narrow, source-hash gated and reversible."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[2] / "ios/tools/export_min_reduction.py"
SPEC = importlib.util.spec_from_file_location("min_reduction_export", TOOL)
assert SPEC is not None and SPEC.loader is not None
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


def source(prefix):
    return f"""#include <metal_stdlib>
using namespace metal;
// Synthetic test source; no game code or data.
float sample_fixture() {{
  float result = spvDescriptorSet0.{prefix}_t_tcb_2A.sample(spvDescriptorSet0.{prefix}_t_tcb_2ASmplr, select(float2(f(x, y), z), float2(0.0), isnan(float2(x, y))), level(integer_lod)).x;
  return result + unrelated.sample(other_sampler, uv, level(0)).x;
}}
"""


class MinReductionExportTests(unittest.TestCase):
    def test_only_unique_slot17_call_changes_and_helper_is_self_contained(self):
        for key, profile in EXPORT.AUDITED.items():
            original = source(profile["prefix"])
            rewritten, payload = EXPORT.transform(original, key)
            self.assertEqual(rewritten.count("// DM_MIN_REDUCTION_HELPER_BEGIN"), 1)
            self.assertNotIn('#include "nvn_reduction_footprint.h"', rewritten)
            self.assertIn("unrelated.sample(other_sampler, uv, level(0)).x", rewritten)
            self.assertIn("level(integer_lod)", original)
            self.assertIn("integer_lod, 0.0f, 1000.0f, false).x", rewritten)
            injected = "\n" + EXPORT.HELPER_BEGIN + payload + EXPORT.HELPER_END
            without_helper = rewritten.replace(injected, "", 1)
            tex = f"spvDescriptorSet0.{profile['prefix']}_t_tcb_2A"
            start = original.index(tex + ".sample(")
            end = EXPORT.close_paren(original, start + len(tex + ".sample"))
            expected = original[start:end + 1]
            new_start = without_helper.index("dm_reduction_sample_lmn_edge(")
            new_end = EXPORT.close_paren(without_helper, new_start + len("dm_reduction_sample_lmn_edge"))
            restored = without_helper[:new_start] + expected + without_helper[new_end + 1:]
            self.assertEqual(restored, original)

    def test_rejects_duplicate_implicit_offset_or_wrong_sampler_calls(self):
        key = next(iter(EXPORT.AUDITED))
        original = source("fp")
        for bad in (original + original, original.replace("level(integer_lod)", "bias(1)"),
                    original.replace("level(integer_lod)", "level(integer_lod), int2(1, 0)"),
                    original.replace("fp_t_tcb_2ASmplr", "differentSampler")):
            with self.subTest(source=bad), self.assertRaises(ValueError):
                EXPORT.transform(bad, key)

    def prepare(self, root):
        cache, libs, out = root / f"cache/b-{EXPORT.CHAIN}", root / f"ios/b-{EXPORT.CHAIN}", root / "min-reduction-v1"
        cache.mkdir(parents=True); libs.mkdir(parents=True)
        audited = {}
        for key, profile in EXPORT.AUDITED.items():
            data = source(profile["prefix"]).encode()
            audited[key] = dict(profile, original_source_sha256=hashlib.sha256(data).hexdigest())
            (cache / (key + ".metal")).write_bytes(data)
            (libs / (key + ".metallib")).write_bytes(b"MTLB-synthetic-original")
            prefix = profile["prefix"]
            meta = {"chain": EXPORT.CHAIN, "key": key, "stage": profile["stage"], "entry": "main0", "bindings": [
                {"kind": "tex", "reg": 17, "msl_name": f"{prefix}_t_tcb_2A", "msl_type": "texture2d<float>"},
                {"kind": "samp_tex", "reg": 17, "msl_name": f"{prefix}_t_tcb_2ASmplr", "msl_type": "sampler"},
            ]}
            (libs / (key + ".json")).write_text(json.dumps(meta))
        return cache, libs, out, audited

    def test_export_records_sha_contract_preserves_originals_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, libs, out, audited = self.prepare(Path(tmp))
            originals = {p: p.read_bytes() for d in (cache, libs) for p in d.iterdir()}
            with patch.object(EXPORT, "AUDITED", audited):
                self.assertEqual(EXPORT.export(cache, libs, out, EXPORT.CHAIN), 2)
                self.assertEqual(EXPORT.export(cache, libs, out, EXPORT.CHAIN), 2)
            for key, profile in audited.items():
                record = json.loads((out / (key + ".json")).read_text())
                self.assertEqual(record["source_sha256"], hashlib.sha256((out / (key + ".metal")).read_bytes()).hexdigest())
                self.assertEqual(record["original_source_sha256"], profile["original_source_sha256"])
                self.assertEqual(record["library_sha256"], hashlib.sha256(b"MTLB-synthetic-original").hexdigest())
                self.assertEqual(record["helper_sha256"], hashlib.sha256(EXPORT.helper_source().encode()).hexdigest())
                self.assertEqual(record["sampler"], EXPORT.SAMPLER)
                self.assertTrue(record["integer_explicit_lod"])
                self.assertEqual((record["entry"], record["reg"], record["calls"]), ("main0", 17, 1))
            for path, data in originals.items():
                self.assertEqual(path.read_bytes(), data)

    def test_fails_closed_on_original_sha_reflection_chain_or_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, libs, out, audited = self.prepare(Path(tmp))
            with self.assertRaises(ValueError):
                EXPORT.export(cache, libs, out, EXPORT.CHAIN) # synthetic source is not an audited original
            self.assertFalse(out.exists())
            with patch.object(EXPORT, "AUDITED", audited):
                with self.assertRaises(ValueError): EXPORT.export(cache, libs, out, "wrong-chain")
                key = next(iter(audited))
                path = libs / (key + ".json")
                correct = path.read_text(); bad = json.loads(correct); bad["bindings"][0]["reg"] = 18
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError): EXPORT.export(cache, libs, out, EXPORT.CHAIN)
                self.assertFalse(out.exists()); path.write_text(correct)
                EXPORT.export(cache, libs, out, EXPORT.CHAIN)
                (out / (key + ".metal")).write_text("changed companion")
                with self.assertRaises(ValueError): EXPORT.export(cache, libs, out, EXPORT.CHAIN)
                with self.assertRaises(ValueError): EXPORT.export(cache, libs, cache, EXPORT.CHAIN)
                with self.assertRaises(ValueError): EXPORT.export(cache, libs, EXPORT.ROOT / "game-assets", EXPORT.CHAIN)


if __name__ == "__main__":
    unittest.main()
