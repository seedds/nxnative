"""Compile and run the launcher's real settings model on macOS; no simulator or UI required."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import settings


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class LauncherStorageTests(unittest.TestCase):
    def test_only_changed_choices_are_stored(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'patched').mkdir()
            (tmp / 'patched/main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # the render-scale row, a range
            with patch.dict(os.environ, {'GFXAPI': 'nvn'}), patch.object(settings.nacp, 'languages', return_value=['en-US', 'ja']):
                (tmp / 'launcher.json').write_text(json.dumps(settings.build('01007EF00011E000', tmp / 'patched', tmp / 'control.nacp')))
                with patch.dict(os.environ, {'SETTINGS_PLATFORM': 'mac'}):   # the Mac window's list: Display mode, the render scale's base size
                    (tmp / 'launcher_mac.json').write_text(json.dumps(settings.build('01007EF00011E000', tmp / 'patched', tmp / 'control.nacp')))
            exe = tmp / 'launcher_storage_check'
            host = REPO / 'dm/host'
            objects = []
            for unit in ('local_users', 'local_save_root', 'save_sync'):
                obj = tmp / (unit + '.o')
                subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', '-fobjc-arc', '-c', str(host / (unit + ('.c' if unit == 'save_sync' else '.m'))), '-o', str(obj)], check=True)
                objects.append(str(obj))
            compile_result = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library', '-module-cache-path', str(tmp / 'module-cache'),
                            '-import-objc-header', str(host / 'local_users.h'), *objects, '-o', str(exe),
                            str(REPO / 'dm/ios/Sources/LauncherSettings.swift'), str(REPO / 'dm/ios/Sources/LauncherRow.swift'), str(REPO / 'dm/ios/Sources/LocalSaveFolder.swift'), str(REPO / 'dm/ios/Sources/SaveSyncReport.swift'), str(REPO / 'dm/mac/Launcher.swift'),
                            str(Path(__file__).with_name('launcher_layout_check.swift')), str(Path(__file__).with_name('launcher_storage_check.swift'))], capture_output=True, text=True, timeout=300)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            r = subprocess.run([str(exe), str(tmp / 'launcher.json'), str(tmp / 'launcher_mac.json')], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('launcher storage: 0 failed', r.stdout)
            users = tmp / 'users.json'
            second = '00000000000000000000000000000002'
            users.write_text(json.dumps({'version': 1, 'users': [{'id': '53574e415449564578626c0000000001', 'nickname': 'Alice'}, {'id': second, 'nickname': 'Bob'}]}))
            for preset in ('', second):
                env = dict(os.environ, DM_USERS_FILE=str(users))
                if preset: env['DM_USER_ID'] = preset
                else: env.pop('DM_USER_ID', None)
                r = subprocess.run([str(exe), '--users', second], capture_output=True, text=True, env=env, timeout=30)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            users.write_text(json.dumps({'version': 1, 'users': [
                {'id': '000000000000000000000000deadbeef', 'nickname': 'Alex'},
                {'id': '010000000000000000000000deadbeef', 'nickname': 'Alex'},
                {'id': '000000000000000000000000abcdef01', 'nickname': 'Alex'},
                {'id': second, 'nickname': 'Bob'}]}))
            env.pop('DM_USER_ID', None)
            r = subprocess.run([str(exe), '--duplicate-names'], capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            env = {k: v for k, v in os.environ.items() if k not in ('DM_USER_ID', 'DM_USERS_FILE', 'DM_NICK')}
            edited = tmp / 'first-launch-users.json'
            r = subprocess.run([str(exe), '--edit-users', str(edited)], capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(json.loads(edited.read_text())['users'][0], {'id': '53574e415449564578626c0000000001', 'nickname': 'Alice'})


if __name__ == '__main__':
    unittest.main()
