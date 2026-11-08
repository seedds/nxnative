"""scripts/resign_ipa.py: what it reads from an IPA, the bundle ID it gives it, choosing the team and the device, the signature it makes (ad-hoc here, so no Apple account or device is
needed) and the stub project, which Xcode must be able to build (skipped without the iOS SDK). Temporary files only in a temporary folder."""
import importlib.util
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('resign_ipa', REPO / 'scripts/resign_ipa.py')
resign = importlib.util.module_from_spec(spec); spec.loader.exec_module(resign)


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def make_ipa(self, bundle_id='org.nxnative.0100535012974000', binary=False):
        info = {'CFBundleIdentifier': bundle_id, 'CFBundleExecutable': 'DmIOS'}
        fmt = plistlib.FMT_BINARY if binary else plistlib.FMT_XML
        ipa = self.tmp / 'game.ipa'
        with zipfile.ZipFile(ipa, 'w') as z:
            z.writestr('Payload/DmIOS.app/Info.plist', plistlib.dumps(info, fmt=fmt))
            z.writestr('Payload/DmIOS.app/romfs.img', b'x' * 100)
        return ipa


class ReadIpa(Base):
    def test_finds_the_app_and_its_size(self):
        top, info, size = resign.read_ipa(self.make_ipa())
        self.assertEqual(top, 'Payload/DmIOS.app')
        self.assertEqual(info['CFBundleIdentifier'], 'org.nxnative.0100535012974000')
        self.assertGreater(size, 100)

    def test_rejects_a_file_that_is_not_an_ipa(self):
        bad = self.tmp / 'bad.ipa'
        bad.write_bytes(b'not a zip')
        with self.assertRaises(resign.Fail):
            resign.read_ipa(bad)

    def test_rejects_an_archive_without_an_app(self):
        ipa = self.tmp / 'empty.ipa'
        with zipfile.ZipFile(ipa, 'w') as z:
            z.writestr('readme.txt', 'x')
        with self.assertRaises(resign.Fail):
            resign.read_ipa(ipa)


class BundleId(Base):
    def test_is_stable_and_names_the_team(self):
        self.assertEqual(resign.new_bundle_id('org.nxnative.0100535012974000', '5K752T46GR'), 'org.nxnative.5k752t46gr.0100535012974000')

    def test_keeps_the_plist_format_and_other_keys(self):
        for binary in (False, True):
            plist = self.tmp / 'Info.plist'
            plist.write_bytes(plistlib.dumps({'CFBundleIdentifier': 'a', 'Other': 1}, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML))
            resign.set_bundle_id(plist, 'b')
            self.assertEqual(plist.read_bytes().startswith(b'bplist'), binary)
            self.assertEqual(plistlib.loads(plist.read_bytes()), {'CFBundleIdentifier': 'b', 'Other': 1})


class Choosing(unittest.TestCase):
    def test_the_only_team(self):
        team = {'teamID': 'T1', 'isFreeProvisioningTeam': True}
        with mock.patch.object(resign, 'find_teams', return_value=[team]):
            self.assertEqual(resign.pick_team(None), team)

    def test_several_teams_or_none_stop(self):
        for teams in ([], [{'teamID': 'T1'}, {'teamID': 'T2'}]):
            with mock.patch.object(resign, 'find_teams', return_value=teams), self.assertRaises(resign.Fail):
                resign.pick_team(None)

    def test_devices(self):
        with mock.patch.object(resign, 'find_devices', return_value=[('U1', 'a')]):
            self.assertEqual(resign.pick_device(None), 'U1')
            with self.assertRaises(resign.Fail):
                resign.pick_device('U2')
        with mock.patch.object(resign, 'find_devices', return_value=[('U1', 'a'), ('U2', 'b')]):
            self.assertEqual(resign.pick_device('U2'), 'U2')
            with self.assertRaises(resign.Fail):
                resign.pick_device(None)

    def test_a_free_team_stops_before_its_app_limit(self):
        team = {'teamID': 'T1', 'isFreeProvisioningTeam': True}
        names = [f'{i}.ipa' for i in range(resign.FREE_TEAM_APPS + 1)]
        with mock.patch.object(resign, 'find_teams', return_value=[team]), mock.patch.object(resign, 'find_devices') as devices, mock.patch('sys.stderr'):
            self.assertEqual(resign.main(names), 1)
        devices.assert_not_called()


class Identity(unittest.TestCase):
    def test_matches_the_profile_certificate(self):
        import hashlib
        sha1 = hashlib.sha1(b'cert').hexdigest().upper()
        text = f'  1) {sha1} "Apple Development: someone"\n'
        self.assertEqual(resign.identity_for({'DeveloperCertificates': [b'other', b'cert']}, text), sha1)
        with self.assertRaises(resign.Fail):
            resign.identity_for({'DeveloperCertificates': [b'other']}, text)


class Signing(Base):
    def test_ad_hoc_signature_carries_the_entitlement_and_seals_the_bundle(self):
        if not shutil.which('codesign'):
            self.skipTest('needs codesign')
        app = self.tmp / 'DmIOS.app'
        app.mkdir()
        exe = app / 'DmIOS'
        shutil.copyfile('/usr/bin/true', exe)
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        (app / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'a.b', 'CFBundleExecutable': 'DmIOS', 'CFBundlePackageType': 'APPL'}))
        (app / 'romfs.img').write_bytes(b'data')
        (app / '_CodeSignature').mkdir()
        (app / '_CodeSignature' / 'stale').write_text('old')
        profile = self.tmp / 'p.mobileprovision'
        profile.write_bytes(b'profile')
        entitlements = self.tmp / 'e.plist'
        entitlements.write_bytes(plistlib.dumps({resign.MEMORY_KEY: True}))

        resign.sign_app(app, '-', entitlements, profile)

        self.assertTrue(resign.read_entitlements(app)[resign.MEMORY_KEY])
        self.assertEqual((app / 'embedded.mobileprovision').read_bytes(), b'profile')
        self.assertFalse((app / '_CodeSignature' / 'stale').exists())
        (app / 'romfs.img').write_bytes(b'changed')
        self.assertNotEqual(subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], capture_output=True).returncode, 0)

    def test_nested_code_is_signed_innermost_first(self):
        inner = self.tmp / 'A.app/Frameworks/F.framework/Libs/l.dylib'
        inner.parent.mkdir(parents=True)
        inner.write_bytes(b'')
        self.assertEqual([p.name for p in resign.nested_code(self.tmp / 'A.app')], ['l.dylib', 'F.framework'])


class LastingProfile(Base):
    def run_with(self, profiles, free=False):
        """provision_lasting with Xcode's answers in order; returns the profile it settled on and how many times it asked."""
        cache = self.tmp / 'cache'
        cache.mkdir(exist_ok=True)
        (cache / 'old.mobileprovision').write_bytes(b'x')
        (cache / 'other.mobileprovision').write_bytes(b'x')
        answers = iter(profiles)
        with mock.patch.object(resign, 'provision', return_value=self.tmp / 'Stub.app') as asked, mock.patch.object(resign, 'read_profile', side_effect=lambda _: next(answers)), mock.patch('builtins.print') as printed:
            _, profile = resign.provision_lasting(self.tmp / 'stub', 'T1', 'a.b', 'U1', True, free, cache)
        self.printed = ' '.join(str(c.args[0]) for c in printed.call_args_list if c.args)
        return profile, asked.call_count, cache

    def test_a_paid_team_replaces_a_free_accounts_profile(self):
        profile, asked, cache = self.run_with([{'UUID': 'old', 'TimeToLive': 7}, {'UUID': 'new', 'TimeToLive': 365}])
        self.assertEqual((profile['UUID'], asked), ('new', 2))
        self.assertFalse((cache / 'old.mobileprovision').exists())
        self.assertTrue((cache / 'other.mobileprovision').exists())   # only that game's profile goes
        self.assertNotIn('[RED]', self.printed)

    def test_a_year_long_profile_is_kept(self):
        profile, asked, cache = self.run_with([{'UUID': 'old', 'TimeToLive': 365}])
        self.assertEqual(asked, 1)
        self.assertTrue((cache / 'old.mobileprovision').exists())

    def test_a_free_team_keeps_its_7_day_profile(self):
        profile, asked, cache = self.run_with([{'UUID': 'old', 'TimeToLive': 7}], free=True)
        self.assertEqual(asked, 1)
        self.assertTrue((cache / 'old.mobileprovision').exists())

    def test_it_says_so_when_apple_still_gives_a_short_profile(self):
        profile, asked, _ = self.run_with([{'UUID': 'old', 'TimeToLive': 7}, {'UUID': 'old2', 'TimeToLive': 7}])
        self.assertEqual(asked, 2)   # one retry, no loop
        self.assertIn('[RED]', self.printed)


class Stub(Base):
    def test_xcode_builds_the_stub_with_the_entitlement_file(self):
        sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], capture_output=True)
        if sdk.returncode:
            self.skipTest('needs the iOS SDK')
        project = resign.write_stub(self.tmp, 'ABCDE12345', 'org.nxnative.abcde12345.test', True)
        self.assertTrue(plistlib.loads((self.tmp / 'Stub.entitlements').read_bytes())[resign.MEMORY_KEY])
        done = subprocess.run(['xcodebuild', '-project', str(project), '-scheme', 'Stub', '-configuration', 'Debug', '-destination', 'generic/platform=iOS',
                               '-derivedDataPath', str(self.tmp / 'derived'), 'CODE_SIGNING_ALLOWED=NO', 'build'], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr[-2000:])
        app = self.tmp / 'derived/Build/Products/Debug-iphoneos/Stub.app'
        self.assertEqual(plistlib.loads((app / 'Info.plist').read_bytes())['CFBundleIdentifier'], 'org.nxnative.abcde12345.test')


if __name__ == '__main__':
    unittest.main()
