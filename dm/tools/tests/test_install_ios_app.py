"""The standalone iOS app installer: bundle and profile checks, device selection, and install/launch gating.

All fixtures are synthetic; no game bundle, Apple account, device or key is read.
"""
from __future__ import annotations

from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
from io import StringIO
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("install_ios_app", ROOT / "scripts/install_ios_app.py")
assert spec is not None and spec.loader is not None
installer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = installer   # dataclasses look the defining module up by name while the class is made (3.14)
spec.loader.exec_module(installer)


class InstallIOSAppTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="install-ios-app-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app_path = self.root / "Fixture.app"
        self.app_path.mkdir()
        with (self.app_path / "Info.plist").open("wb") as stream:
            plistlib.dump({
                "CFBundleIdentifier": "com.example.fixture",
                "CFBundleDisplayName": "Fixture",
                "CFBundleShortVersionString": "1.0",
            }, stream)
        (self.app_path / "embedded.mobileprovision").write_bytes(b"fixture")
        self.team = "FIXTURETEAM"
        self.udid = "00000000-0000-0000-0000-000000000001"
        self.now = datetime(2026, 10, 29, 12, 0, tzinfo=timezone.utc)
        self.profile = {
            "ExpirationDate": self.now + timedelta(days=30),
            "ProvisionedDevices": [self.udid],
            "Platform": ["iOS"],
            "TeamIdentifier": [self.team],
            "Entitlements": {"application-identifier": self.team + ".com.example.fixture"},
        }

    def signing(self, verify_return: int = 0, verify_stderr: str = ""):
        def run(args, **kwargs):
            if args[0] == "codesign" and args[1] == "-dv":
                return subprocess.CompletedProcess(args, 0, "", f"Identifier=com.example.fixture\nTeamIdentifier={self.team}\n")
            if args[0] == "codesign" and args[1] == "--verify":
                return subprocess.CompletedProcess(args, verify_return, "", verify_stderr)
            raise AssertionError(f"unexpected command: {args}")
        return run

    def inspected(self):
        with patch.object(installer, "_decode_profile", return_value=self.profile), \
                patch.object(installer.subprocess, "run", side_effect=self.signing()):
            return installer.inspect_app(self.app_path, now=self.now)

    def test_ready_bundle_reports_identity_expiry_and_devices(self):
        app = self.inspected()
        self.assertIsNone(app.error)
        self.assertEqual((app.bundle_id, app.name, app.version), ("com.example.fixture", "Fixture", "1.0"))
        self.assertEqual(app.provisioned, frozenset({self.udid}))
        self.assertTrue(app.installable)

    def test_missing_profile_wrong_app_and_expired_profile_are_rejected(self):
        (self.app_path / "embedded.mobileprovision").unlink()
        missing = self.inspected()
        self.assertIn("missing", missing.error or "")

        (self.app_path / "embedded.mobileprovision").write_bytes(b"fixture")
        wrong = dict(self.profile, Entitlements={"application-identifier": self.team + ".com.example.other"})
        with patch.object(installer, "_decode_profile", return_value=wrong), \
                patch.object(installer.subprocess, "run", side_effect=AssertionError("must stop before codesign")):
            app = installer.inspect_app(self.app_path, now=self.now)
        self.assertIn("different app", app.error or "")

        expired = dict(self.profile, ExpirationDate=self.now - timedelta(seconds=1))
        with patch.object(installer, "_decode_profile", return_value=expired), \
                patch.object(installer.subprocess, "run", side_effect=AssertionError("must stop before codesign")):
            app = installer.inspect_app(self.app_path, now=self.now)
        self.assertIn("expired", app.error or "")

    def test_signature_trust_warning_is_disclosed_but_other_errors_fail(self):
        with patch.object(installer, "_decode_profile", return_value=self.profile), \
                patch.object(installer.subprocess, "run", side_effect=self.signing(1, "app: CSSMERR_TP_NOT_TRUSTED\nIn architecture: arm64\n")):
            app = installer.inspect_app(self.app_path, now=self.now)
        self.assertIsNone(app.error)
        self.assertIn("certificate trust", app.note)

        with patch.object(installer, "_decode_profile", return_value=self.profile), \
                patch.object(installer.subprocess, "run", side_effect=self.signing(1, "invalid code signature")):
            app = installer.inspect_app(self.app_path, now=self.now)
        self.assertIsNotNone(app.error)

    def device_payload(self):
        return {"result": {"devices": [
            {"hardwareProperties": {"udid": "phone-b", "deviceType": "iPhone", "reality": "physical", "marketingName": "iPhone 18"},
             "deviceProperties": {"name": "Second"}, "connectionProperties": {"tunnelState": "disconnected"}},
            {"hardwareProperties": {"udid": "phone-a", "deviceType": "iPhone", "reality": "physical", "marketingName": "iPhone 17 Pro"},
             "deviceProperties": {"name": "First"}, "connectionProperties": {"tunnelState": "connected"}},
            {"hardwareProperties": {"udid": "sim", "deviceType": "iPhone", "reality": "simulated", "marketingName": "iPhone Simulator"},
             "deviceProperties": {"name": "Simulator"}, "connectionProperties": {"tunnelState": "connected"}},
            {"hardwareProperties": {"udid": "tablet", "deviceType": "iPad", "reality": "physical", "marketingName": "iPad Pro"},
             "deviceProperties": {"name": "Tablet"}, "connectionProperties": {"tunnelState": "disconnected"}},
            {"hardwareProperties": {"udid": "tv", "deviceType": "AppleTV", "reality": "physical", "marketingName": "Apple TV"},
             "deviceProperties": {"name": "Living Room"}, "connectionProperties": {"tunnelState": "connected"}},
        ]}}

    def test_device_discovery_lists_only_physical_devices_connected_first(self):
        response = subprocess.CompletedProcess([], 0, json.dumps(self.device_payload()), "")
        with patch.object(installer.subprocess, "run", return_value=response):
            devices = installer.discover_devices()
        self.assertEqual([device.name for device in devices], ["First", "Second", "Tablet"])
        self.assertEqual([device.connected for device in devices], [True, False, False])

    def test_choose_device_requires_one_connected_match(self):
        devices = [installer.Device("First", "a", "iPhone", True), installer.Device("Second", "b", "iPhone", True)]
        with self.assertRaisesRegex(ValueError, "more than one"):
            installer.choose_device(devices, None)
        self.assertEqual(installer.choose_device(devices, "Second").udid, "b")
        with self.assertRaisesRegex(ValueError, "not connected"):
            installer.choose_device([installer.Device("Off", "c", "iPhone", False)], "Off")
        with self.assertRaisesRegex(ValueError, "no iPhone"):
            installer.choose_device([], None)

    def test_check_for_device_rejects_unprovisioned_and_expired_builds(self):
        device = installer.Device("First", self.udid, "iPhone", True)
        ready = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({self.udid}))
        installer.check_for_device(ready, device)
        other = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({"other"}))
        with self.assertRaisesRegex(ValueError, "not provisioned"):
            installer.check_for_device(other, device)
        expired = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now - timedelta(seconds=1), frozenset({self.udid}))
        with self.assertRaisesRegex(ValueError, "expired"):
            installer.check_for_device(expired, device)

    def test_install_failure_never_launches_and_launch_failure_is_returned(self):
        app = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({self.udid}))
        device = installer.Device("First", self.udid, "iPhone", True)
        with patch.object(installer.subprocess, "run", return_value=subprocess.CompletedProcess([], 9)) as run:
            self.assertNotEqual(installer.install_and_launch(app, device), 0)
            self.assertEqual(run.call_count, 1)
        with patch.object(installer.subprocess, "run", side_effect=[
            subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 8),
        ]) as run:
            self.assertNotEqual(installer.install_and_launch(app, device), 0)
            self.assertEqual(run.call_count, 2)

    def test_list_does_not_install(self):
        app = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({self.udid}))
        with patch.object(installer, "inspect_app", return_value=app), \
                patch.object(installer, "discover_devices", return_value=[installer.Device("First", self.udid, "iPhone", True)]), \
                patch.object(installer.subprocess, "run", side_effect=AssertionError("--list must not install")), \
                redirect_stdout(StringIO()) as output:
            self.assertEqual(installer.main([str(self.app_path), "--list"]), 0)
        self.assertIn("com.example.fixture", output.getvalue())

    def test_yes_installs_then_launches(self):
        app = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({self.udid}))
        device = installer.Device("First", self.udid, "iPhone", True)
        commands = []

        def run(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        with patch.object(installer, "inspect_app", return_value=app), \
                patch.object(installer, "discover_devices", return_value=[device]), \
                patch.object(installer.subprocess, "run", side_effect=run), redirect_stdout(StringIO()) as output:
            self.assertEqual(installer.main([str(self.app_path), "--yes"]), 0)
        self.assertEqual(len(commands), 2)
        self.assertEqual(commands[0][:5], ["xcrun", "devicectl", "device", "install", "app"])
        self.assertEqual(commands[1][:5], ["xcrun", "devicectl", "device", "process", "launch"])
        self.assertIn("Installed and launched", output.getvalue())

    def test_no_launch_and_cancelled_confirmation_do_not_launch(self):
        app = installer.App(self.app_path, "com.example.fixture", "Fixture", "1.0", self.now + timedelta(days=1), frozenset({self.udid}))
        device = installer.Device("First", self.udid, "iPhone", True)
        commands = []

        def run(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        with patch.object(installer, "inspect_app", return_value=app), \
                patch.object(installer, "discover_devices", return_value=[device]), \
                patch.object(installer.subprocess, "run", side_effect=run), redirect_stdout(StringIO()):
            self.assertEqual(installer.main([str(self.app_path), "--yes", "--no-launch"]), 0)
        self.assertEqual(len(commands), 1)

        commands.clear()
        with patch.object(installer, "inspect_app", return_value=app), \
                patch.object(installer, "discover_devices", return_value=[device]), \
                patch("builtins.input", return_value="n"), \
                patch.object(installer.subprocess, "run", side_effect=run), \
                redirect_stdout(StringIO()) as output:
            self.assertEqual(installer.main([str(self.app_path)]), 0)
        self.assertEqual(commands, [])
        self.assertIn("Cancelled", output.getvalue())


if __name__ == "__main__":
    unittest.main()
