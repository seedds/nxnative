import importlib.util
import tempfile
import unittest
from pathlib import Path


IOS = Path(__file__).resolve().parents[1]
ZA_PROFILE = IOS.parent / "games/0100F43008C44000"
SCRIPT = IOS / "tools/stage_profile_env.py"
spec = importlib.util.spec_from_file_location("stage_profile_env", SCRIPT)
profile_env = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(profile_env)


class ProfileEnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.profile = self.root / "profile"
        self.profile.mkdir()
        self.output = self.root / "Gen/dm/env.txt"

    def tearDown(self):
        self.temp.cleanup()

    def test_profile_default_is_staged_when_no_override_exists(self):
        default = self.profile / "ios.env"
        default.write_bytes(b"DM_PRESENT_HZ=120\n")
        self.assertEqual(profile_env.stage(self.profile, self.output, {}), default)
        self.assertEqual(self.output.read_bytes(), default.read_bytes())

    def test_explicit_file_has_priority(self):
        (self.profile / "ios.env").write_text("DM_PRESENT_HZ=120\n")
        explicit = self.root / "diagnostic.env"
        explicit.write_text("DM_PRESENT_HZ=60\n")
        self.assertEqual(
            profile_env.stage(self.profile, self.output, {"DM_ENV_FILE": str(explicit)}),
            explicit,
        )
        self.assertEqual(self.output.read_text(), "DM_PRESENT_HZ=60\n")

    def test_explicit_empty_value_disables_profile_default(self):
        (self.profile / "ios.env").write_text("DM_PRESENT_HZ=120\n")
        self.output.parent.mkdir(parents=True)
        self.output.write_text("DM_PRESENT_HZ=30\n")
        self.assertIsNone(profile_env.stage(self.profile, self.output, {"DM_ENV_FILE": ""}))
        self.assertFalse(self.output.exists())

    def test_missing_explicit_file_fails_closed(self):
        with self.assertRaisesRegex(FileNotFoundError, "iOS environment file not found"):
            profile_env.stage(
                self.profile,
                self.output,
                {"DM_ENV_FILE": str(self.root / "missing.env")},
            )

    def test_device_documents_keep_precedence_over_bundled_defaults(self):
        source = (IOS / "Sources/main.m").read_text()
        documents = 'for (NSString *ef in @[[doc stringByAppendingPathComponent:@"env.txt"]'
        bundle = '[[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"env.txt"]])'
        self.assertLess(source.index(documents), source.index(bundle))
        self.assertIn("if (!getenv(k.UTF8String))", source)

    def test_za_profile_has_only_release_defaults(self):
        values = [
            line
            for line in (ZA_PROFILE / "ios.env").read_text().splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(
            values,
            [
                "DM_PRESENT_INTERVAL=game",
                "DM_ZA_SKILL_LAYOUT=1",
                "DM_DISPLAY_HZ=120",
                "DM_PRESENT_HZ=120",
                "DM_ZA_FPS_MODE=dynamic60",
            ],
        )

    def test_build_script_stages_the_selected_profile_environment(self):
        source = (IOS / "tools/build_ipa.sh").read_text()
        self.assertIn(
            'ENV_SOURCE=$(python3 tools/stage_profile_env.py "$GD" Gen/dm/env.txt)',
            source,
        )


if __name__ == "__main__":
    unittest.main()
