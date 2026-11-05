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
    def test_players_bridge_without_ui(self):
        """Exercise opt-in selection and a missing dylib without NSApplication or preferences."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = tmp / 'players_check.swift'
            source.write_text('''import Foundation
var calls: [Int32] = []
@_cdecl("dm_launcher_players_view")
func playersViewStub(_ enabled: Int32) -> UnsafeMutableRawPointer? {
    calls.append(enabled)
    return nil
}
@_cdecl("dm_launcher_players_finish")
func playersFinishStub(_ play: Int32) { fatalError("C owns finish, not Swift") }
@main struct Check {
    static func main() {
        // An empty settings list reads/writes no preferences.
        let model = LauncherModel([])
        assert(!LauncherPlayers.enabled(model, environment: [:]))
        assert(LauncherPlayers.enabled(model, environment: ["DM_PLAYER_SETUP": "1"]))
        assert(!LauncherPlayers.enabled(model, environment: ["DM_PLAYER_SETUP": "0"]))
        model.values["DM_PLAYER_SETUP"] = "0"
        assert(!LauncherPlayers.enabled(model, environment: ["DM_PLAYER_SETUP": "1"]))
        model.values["DM_PLAYER_SETUP"] = "1"
        assert(LauncherPlayers.enabled(model, environment: [:]))
        assert(LauncherPlayers.view(enabled: true) == nil)
        assert(LauncherPlayers.view(enabled: false) == nil)
        assert(calls == [1, 0])
        print("launcher players: passed without UI")
    }
}
''')
            exe = tmp / 'players_check'
            result = subprocess.run([
                'xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
                '-module-cache-path', str(tmp / 'module-cache'), '-o', str(exe),
                str(REPO / 'dm/ios/Sources/LauncherSettings.swift'),
                str(REPO / 'dm/ios/Sources/LauncherRow.swift'),
                str(REPO / 'dm/mac/Launcher.swift'), str(source),
            ], capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('launcher players: passed without UI', result.stdout)

    def test_players_hosted_layout(self):
        """Measure the actual representable inside SwiftUI, not just a detached AppKit panel."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = tmp / 'players_layout_check'
            bridge = tmp / 'players_bridge.m'
            bridge.write_text('''#import "players_mac.h"
static DmMacPlayers *players;
void *test_players_view(void) {
    players = [DmMacPlayers new];
    [players beginConfiguration];
    [players sync:@[]];
    return (__bridge void *)[players configurationView];
}
void test_players_change(void) {
    NSStackView *panel = (NSStackView *)[players configurationView];
    for (NSView *child in panel.arrangedSubviews)
        if ([child isKindOfClass:NSButton.class] &&
            [((NSButton *)child).title isEqualToString:@"Add input device"])
            [((NSButton *)child) performClick:nil];
}
''')
            objects = []
            for source in [bridge, REPO / 'dm/host/players_mac.m']:
                obj = tmp / (source.stem + '.o')
                result = subprocess.run([
                    'xcrun', 'clang', '-fobjc-arc', '-I', str(REPO / 'dm/host'),
                    '-c', str(source), '-o', str(obj),
                ], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                objects.append(str(obj))
            result = subprocess.run([
                'xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
                '-module-cache-path', str(tmp / 'module-cache'), '-o', str(exe),
                str(REPO / 'dm/ios/Sources/LauncherSettings.swift'),
                str(REPO / 'dm/ios/Sources/LauncherRow.swift'),
                str(REPO / 'dm/mac/Launcher.swift'),
                str(Path(__file__).with_name('launcher_players_layout_check.swift')),
                *objects, '-framework', 'GameController',
            ], capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('hosted layout passed without visible windows', result.stdout)

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
            compile_result = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library', '-module-cache-path', str(tmp / 'module-cache'),
                            '-o', str(exe), str(REPO / 'dm/ios/Sources/LauncherSettings.swift'), str(REPO / 'dm/ios/Sources/LauncherRow.swift'), str(REPO / 'dm/mac/Launcher.swift'),
                            str(Path(__file__).with_name('launcher_layout_check.swift')), str(Path(__file__).with_name('launcher_storage_check.swift'))], capture_output=True, text=True, timeout=300)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            r = subprocess.run([str(exe), str(tmp / 'launcher.json'), str(tmp / 'launcher_mac.json')], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('launcher storage: 0 failed', r.stdout)


if __name__ == '__main__':
    unittest.main()
