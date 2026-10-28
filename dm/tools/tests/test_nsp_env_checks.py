"""NSP script early environment checks (scripts/build_ipa_from_nsp.py): a missing iOS SDK stops an iOS build before extraction (not a Mac-only one), and an x86_64 Python under
Rosetta stops the build up front. Every input (xcrun, sysctl, PATH) is mocked: no Xcode or Rosetta needed."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


def fake_runner(sdk_rc=0, translated='0', calls=None):
    def run(cmd, **kw):
        if calls is not None: calls.append(list(cmd))
        if cmd[0] == 'xcrun' and '--show-sdk-version' in cmd: return subprocess.CompletedProcess(cmd, sdk_rc, '27.1\n' if not sdk_rc else '', '')
        if cmd[0] == 'sysctl': return subprocess.CompletedProcess(cmd, 0, translated + '\n', '')
        return subprocess.CompletedProcess(cmd, 0, '', '')   # xcrun metal --version
    return run


class Prereqs(unittest.TestCase):
    def check(self, mac, sdk_rc, calls):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp, '_importable', lambda m: True), \
                mock.patch.object(nsp.shutil, 'which', lambda *a, **k: '/x'), mock.patch.object(nsp.subprocess, 'run', fake_runner(sdk_rc, calls=calls)):
            xlate = Path(d) / 'dmsh_xlate'; xlate.write_text('')
            nsp.check_prereqs({'PATH': '/usr/bin'}, xlate, mac)

    def test_ios_without_sdk_stops(self):   # before the Metal compiler check, whose message would blame the Metal toolchain
        calls = []
        with self.assertRaisesRegex(nsp.BuildError, r'iOS SDK is not installed.*Components.*downloadPlatform iOS'): self.check(False, 1, calls)
        self.assertFalse(any('metal' in c for c in calls))

    def test_ios_with_sdk_passes(self):
        calls = []; self.check(False, 0, calls)
        self.assertIn(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], calls)

    def test_mac_does_not_check_the_ios_sdk(self):   # a Mac-only build needs no iOS platform
        calls = []; self.check(True, 1, calls)
        self.assertFalse(any('iphoneos' in c for c in calls))


class Rosetta(unittest.TestCase):
    def check(self, translated, platform='darwin'):
        with mock.patch.object(nsp.sys, 'platform', platform), mock.patch.object(nsp.subprocess, 'run', fake_runner(translated=translated)):
            nsp.check_native_python()

    def test_translated_stops(self):
        with self.assertRaisesRegex(nsp.BuildError, r'running under Rosetta.*arm64 Python'): self.check('1')

    def test_native_passes(self): self.check('0')

    def test_missing_key_passes(self): self.check('')   # an Intel Mac has no sysctl.proc_translated

    def test_other_systems_are_not_asked(self):
        with mock.patch.object(nsp.sys, 'platform', 'linux'), mock.patch.object(nsp.subprocess, 'run', side_effect=AssertionError): nsp.check_native_python()


if __name__ == '__main__': unittest.main()
