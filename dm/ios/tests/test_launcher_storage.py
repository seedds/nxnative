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
sys.path.insert(0, str(REPO / 'dm/mac'))
sys.path.insert(0, str(REPO / 'dm/games'))
import pack_app
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
            exe, sync, sync_apple, km = tmp / 'launcher_storage_check', tmp / 'save_sync.o', tmp / 'save_sync_apple.o', tmp / 'keymap_bridge.o'
            # the window's cloud-save check calls the host's save_sync_report, and its keyboard map keymap_bridge.c's dm_km_*, which the app links as save_sync.o + save_sync_apple.o and keymap_bridge.o (dm/mac/pack_app.py)
            for obj, src in ((sync, REPO / 'dm/host/save_sync.c'), (sync_apple, REPO / 'dm/host/save_sync_apple.m'), (km, REPO / 'dm/mac/keymap_bridge.c')):
                extra = ['-fobjc-arc'] if src.suffix == '.m' else []
                compile_result = subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', *extra, '-I', str(REPO / 'dm/host'), '-c', '-o', str(obj), str(src)], capture_output=True, text=True, timeout=300)
                self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            compile_result = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library', '-module-cache-path', str(tmp / 'module-cache'),
                            '-o', str(exe), *map(str, pack_app.swift_sources()),
                            str(Path(__file__).with_name('launcher_layout_check.swift')), str(Path(__file__).with_name('launcher_storage_check.swift')), str(sync), str(sync_apple), str(km),
                            '-framework', 'Foundation'],
                            capture_output=True, text=True, timeout=300)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            r = subprocess.run([str(exe), str(tmp / 'launcher.json'), str(tmp / 'launcher_mac.json')], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('launcher storage: 0 failed', r.stdout)
            self.assertNotIn('launcher: save sync:', r.stderr)


if __name__ == '__main__':
    unittest.main()
