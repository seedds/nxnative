"""Run build_ipa.sh's path setup and entitlement steps without Xcode or game files."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
SCRIPT = (REPO / 'dm/ios/tools/build_ipa.sh').read_text()
START = SCRIPT[:SCRIPT.index('[ -n "${GAME:-}" ]')]
PREPARE = SCRIPT[SCRIPT.index('if [ "$PREPARE_XCODE" = YES ]; then'):SCRIPT.index('\nexport DM_SCHEME_NAME')]
XCODE = SCRIPT[SCRIPT.index('# A prepared Xcode project is signed'):SCRIPT.index('# DM_COPY_ROMFS=NO also for the prepared project')]
SIGNING = SCRIPT[SCRIPT.index('# The entitlements of an unsigned IPA'):SCRIPT.index('\nP=Payload/DmIOS.app\n')]
CAN_SIGN = sys.platform == 'darwin' and shutil.which('codesign') and shutil.which('cc')
CUSTOM = {'com.apple.developer.kernel.extended-virtual-addressing': True}
DEFAULT = {'com.apple.developer.kernel.increased-memory-limit': True}


class EntitlementsTests(unittest.TestCase):
    def stage(self, prepare, kind):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            caller = root / "caller's $ directory `literal`"
            ios = root / 'repository/dm/ios'
            project = root / 'prepared project'
            app = root / 'DmIOS.app'
            package = root / 'package'
            for folder in (caller, ios / 'tools', ios / 'Sources', project / 'Gen', app, package):
                folder.mkdir(parents=True)
            (ios / 'project.yml').touch()
            custom = caller / "custom $ ' `memory`.plist"
            custom.write_bytes(plistlib.dumps(CUSTOM))
            # A same-named file in the script's directory must not replace the caller's choice.
            for folder in (ios, project):
                (folder / custom.name).write_bytes(plistlib.dumps({'wrong-file': True}))
            env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
            env.update(PREPARE_XCODE='YES' if prepare else 'NO', W=str(root), GAME='fixture',
                       DM_DISPLAY_NAME='Fixture', DM_XCODE_PROJECT_DIR=str(project), APP=str(app), O=str(package))
            if kind == 'relative':
                env['DM_ENTITLEMENTS'] = custom.name
            elif kind == 'dotdot':
                (caller / 'nested').mkdir()
                env['DM_ENTITLEMENTS'] = './nested/../' + custom.name
            elif kind == 'absolute':
                env['DM_ENTITLEMENTS'] = str(custom)
            elif kind == 'empty':
                env['DM_ENTITLEMENTS'] = ''
            elif kind == 'missing':
                custom.unlink()
                env['DM_ENTITLEMENTS'] = custom.name
            if not prepare:
                source = root / 'main.c'
                source.write_text('int main(void) { return 0; }\n')
                subprocess.run(['cc', '-o', str(app / 'DmIOS'), str(source)], check=True, capture_output=True)
            # Keep the production cd commands and consumers; omit only unrelated generation/build steps.
            script = ios / 'tools/build_ipa.sh'
            script.write_text(START + PREPARE + '\n' + (XCODE if prepare else SIGNING))
            result = subprocess.run(['/bin/bash', str(script)], cwd=caller, env=env, capture_output=True, text=True)
            if kind == 'missing':
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('copy DM_ENTITLEMENTS', result.stdout + result.stderr)
                return
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            entitlements = (project / 'Gen' if prepare else package) / 'DmIOS.entitlements'
            expected = None if kind == 'empty' else DEFAULT if kind == 'unset' else CUSTOM
            self.assertEqual(entitlements.exists(), expected is not None)
            if expected is not None:
                self.assertEqual(plistlib.loads(entitlements.read_bytes()), expected)
            if prepare:
                config = project / 'Gen/dm.xcconfig'
                self.assertEqual(config.exists(), expected is not None)
                if expected is not None:
                    self.assertIn('CODE_SIGN_ENTITLEMENTS = Gen/DmIOS.entitlements', config.read_text())
            else:
                shown = subprocess.run(['codesign', '-d', '--entitlements', '-', '--xml', str(app / 'DmIOS')], capture_output=True)
                actual = plistlib.loads(shown.stdout) if shown.stdout.strip() else None
                self.assertEqual(actual, expected)

    def test_prepared_project_entitlements(self):
        for kind in ('relative', 'dotdot', 'absolute', 'unset', 'empty', 'missing'):
            with self.subTest(kind=kind):
                self.stage(True, kind)

    @unittest.skipUnless(CAN_SIGN, 'needs macOS codesign and a C compiler')
    def test_unsigned_executable_entitlements(self):
        for kind in ('relative', 'dotdot', 'absolute', 'unset', 'empty', 'missing'):
            with self.subTest(kind=kind):
                self.stage(False, kind)


if __name__ == '__main__':
    unittest.main()
