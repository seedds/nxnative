#!/usr/bin/env python3
"""Packaging preflight regressions for profile-required NVN companions."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


IOS = Path(__file__).resolve().parents[1]
TOOL = IOS / "tools/validate_shader_companions.py"
SPEC = importlib.util.spec_from_file_location("validate_shader_companions", TOOL)
VALIDATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(VALIDATOR)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class ShaderCompanionValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.libraries = root / "b-test"
        self.libraries.mkdir()
        self.profile = root / "ios_companions.json"
        self.keys = {
            "nonpoint": "1111111111111111_b",
            "layered": "2222222222222222_b",
            "sampler": "3333333333333333_b",
            "zero": "4444444444444444_b",
            "atomic": "5555555555555555_b",
        }
        for key in self.keys.values():
            (self.libraries / (key + ".metallib")).write_bytes(("library-" + key).encode())
            (self.libraries / (key + ".json")).write_bytes(("reflection-" + key).encode())

        self._manifest("nonpoint-v1", None, self.keys["nonpoint"], explicit_source=False)
        self._manifest("layered-v1", "layered-slices-v1", self.keys["layered"], explicit_source=False)
        self._manifest("sampler-limit-v1", "sampler-limit-v1", self.keys["sampler"], limit=16)
        self._manifest("guarded-index-v1", "guarded-index-v1", self.keys["nonpoint"],
                       original_directory="nonpoint-v1")
        self._manifest("zero-image-v1", "zero-image-clear-v1", self.keys["zero"])

        atomic = self.libraries / "atomic-v1"
        atomic.mkdir()
        key = self.keys["atomic"]
        source = b"bounded atomic source"
        (atomic / (key + ".metal")).write_bytes(source)
        (atomic / (key + ".json")).write_text(json.dumps({
            "schema": 1, "chain": "test", "kind": "bounded-image-atomics-v1",
            "source_sha256": digest(source),
            "library_sha256": digest((self.libraries / (key + ".metallib")).read_bytes()),
        }))

        requirements = {
            "schema": 1,
            "manifests": [
                {"directory": "nonpoint-v1", "required_records": [self.keys["nonpoint"]]},
                {"directory": "layered-v1", "kind": "layered-slices-v1",
                 "required_records": [self.keys["layered"]]},
                {"directory": "sampler-limit-v1", "kind": "sampler-limit-v1", "limit": 16,
                 "required_records": [self.keys["sampler"]]},
                {"directory": "guarded-index-v1", "kind": "guarded-index-v1",
                 "original_directory": "nonpoint-v1", "required_records": [self.keys["nonpoint"]]},
                {"directory": "zero-image-v1", "kind": "zero-image-clear-v1",
                 "required_records": [self.keys["zero"]]},
            ],
            "records": [{"directory": "atomic-v1", "kind": "bounded-image-atomics-v1",
                         "required_records": [self.keys["atomic"]]}],
        }
        self.profile.write_text(json.dumps(requirements))

    def tearDown(self):
        self.temp.cleanup()

    def _manifest(self, directory, kind, key, explicit_source=True, limit=None, original_directory=None):
        where = self.libraries / directory
        where.mkdir()
        source = ("source-" + directory).encode()
        name = key + ".metal"
        (where / name).write_bytes(source)
        record = {
            "source_sha256": digest(source),
            "base_library_sha256": digest((self.libraries / (key + ".metallib")).read_bytes()),
            "reflection_sha256": digest((self.libraries / (key + ".json")).read_bytes()),
        }
        if explicit_source:
            record["source"] = name
        if original_directory:
            record["original_source_sha256"] = digest(
                (self.libraries / original_directory / name).read_bytes())
        manifest = {"schema": 1, "chain": "test", "shaders": {key: record}}
        if kind is not None:
            manifest["kind"] = kind
        if limit is not None:
            manifest["limit"] = limit
        (where / "manifest.json").write_text(json.dumps(manifest))

    def test_complete_profile_passes(self):
        self.assertEqual(VALIDATOR.validate(self.profile, self.libraries, "test"), (5, 5, 1))

    def test_missing_manifest_fails_closed(self):
        (self.libraries / "layered-v1/manifest.json").unlink()
        with self.assertRaisesRegex(VALIDATOR.CompanionError, "missing layered-v1 manifest"):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_manifest_chain_kind_and_limit_are_exact(self):
        path = self.libraries / "sampler-limit-v1/manifest.json"
        for field, value, reason in (("chain", "other", "schema/chain"),
                                     ("kind", "other", "manifest kind"),
                                     ("limit", 17, "manifest limit")):
            original = path.read_text()
            manifest = json.loads(original)
            manifest[field] = value
            path.write_text(json.dumps(manifest))
            with self.subTest(field=field), self.assertRaisesRegex(VALIDATOR.CompanionError, reason):
                VALIDATOR.validate(self.profile, self.libraries, "test")
            path.write_text(original)

    def test_required_record_cannot_be_absent_or_unsupported(self):
        path = self.libraries / "zero-image-v1/manifest.json"
        original = json.loads(path.read_text())
        manifest = json.loads(path.read_text())
        manifest["shaders"] = {}
        path.write_text(json.dumps(manifest))
        with self.assertRaises(VALIDATOR.CompanionError):
            VALIDATOR.validate(self.profile, self.libraries, "test")
        manifest = original
        manifest["shaders"][self.keys["zero"]]["supported"] = False
        path.write_text(json.dumps(manifest))
        with self.assertRaises(VALIDATOR.CompanionError):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_every_emitted_source_and_hash_is_checked(self):
        (self.libraries / "layered-v1" / (self.keys["layered"] + ".metal")).unlink()
        with self.assertRaisesRegex(VALIDATOR.CompanionError, "missing or unreadable companion file"):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_source_name_must_match_its_shader_record(self):
        path = self.libraries / "zero-image-v1/manifest.json"
        manifest = json.loads(path.read_text())
        record = manifest["shaders"][self.keys["zero"]]
        original = self.libraries / "zero-image-v1" / record["source"]
        other = self.libraries / "zero-image-v1/other.metal"
        other.write_bytes(original.read_bytes())
        record["source"] = other.name
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(VALIDATOR.CompanionError, "source name does not match"):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_guarded_original_source_is_checked(self):
        original = self.libraries / "nonpoint-v1" / (self.keys["nonpoint"] + ".metal")
        original.write_bytes(b"changed")
        with self.assertRaisesRegex(VALIDATOR.CompanionError, "source_sha256 mismatch"):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_atomic_record_and_source_are_mandatory(self):
        (self.libraries / "atomic-v1" / (self.keys["atomic"] + ".json")).unlink()
        with self.assertRaisesRegex(VALIDATOR.CompanionError, "missing atomic-v1 record"):
            VALIDATOR.validate(self.profile, self.libraries, "test")

    def test_za_profile_names_observed_a10x_requirements(self):
        profile = IOS.parent / "games/0100F43008C44000"
        self.assertIn("ios_companions\tios_companions.json", (profile / "game.tsv").read_text())
        requirements = json.loads((profile / "ios_companions.json").read_text())
        manifests = {item["directory"]: item for item in requirements["manifests"]}
        self.assertEqual(set(manifests), {"nonpoint-v1", "layered-v1", "sampler-limit-v1",
                                         "guarded-index-v1", "zero-image-v1"})
        self.assertEqual(set(manifests["layered-v1"]["required_records"]), {
            "57246f9a04af8529_b", "d7d72bbbb0e32517_b",
            "b29f08619f379a83_b", "5b931243ad28aeeb_b",
        })
        self.assertIn("a6c06cda878a4fe0_b", manifests["sampler-limit-v1"]["required_records"])
        self.assertIn("ce7eacf81c6e4ffc_b", manifests["guarded-index-v1"]["required_records"])
        self.assertIn("5b107ddfd06551fc_b", manifests["zero-image-v1"]["required_records"])
        self.assertEqual(requirements["records"][0]["required_records"], ["2bd5037003980716_b"])

    def test_build_preflight_runs_before_generation_and_packing(self):
        source = (IOS / "tools/build_ipa.sh").read_text()
        check = 'python3 tools/validate_shader_companions.py "$GD/$COMPANION_REQUIREMENTS"'
        self.assertLess(source.index(check), source.index('t "generate Gen/'))
        self.assertLess(source.index(check), source.index("python3 tools/pack_ipa.py"))
        self.assertIn('die "profile companion requirements are missing:', source)


if __name__ == "__main__":
    unittest.main()
