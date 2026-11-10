#!/usr/bin/env python3
"""Build the screen rate check (dm/ios/tests/screen_rate_check.m) as an unsigned iOS app.

Requires Xcode and XcodeGen; GAME (or DM_GAME) selects the profile whose constants the
front end's sources compile with. No game files, keys or signing are used. --output must
name a new directory; every product stays under it. Prints the app's path: install and
open it on a simulator (--simulator) or, once signed, on a device, then read
Documents/screen_rate.log in its container.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
LANDSCAPE = ['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new scratch directory for the project and build products')
    parser.add_argument('--simulator', action='store_true', help='build for the iOS Simulator instead of a device')
    args = parser.parse_args()
    game = os.environ.get('GAME') or os.environ.get('DM_GAME')
    if not game:
        parser.error('set GAME (or DM_GAME) to a base game title ID')
    root = args.output.resolve()
    root.mkdir(parents=True)  # Never overwrite an existing project or build.
    include = root / 'Include'
    include.mkdir()
    subprocess.run([sys.executable, '-B', str(REPO / 'dm/games/game.py'), 'header', game, str(include / 'game_profile.h')], check=True)
    scene = {'UISceneConfigurationName': 'Default', 'UISceneDelegateClassName': 'RateScene'}
    target = {
        'type': 'application', 'platform': 'iOS',
        'sources': [{'path': str(REPO / 'dm/ios/tests/screen_rate_check.m')}],
        'dependencies': [{'sdk': name + '.framework'} for name in ('UIKit', 'QuartzCore', 'Metal', 'AVFAudio', 'GameController')],
        'settings': {'base': {
            'PRODUCT_BUNDLE_IDENTIFIER': 'dev.nxnative.screen-rate-check', 'ENABLE_DEBUG_DYLIB': 'NO',
            'HEADER_SEARCH_PATHS': [str(REPO / 'dm/host'), str(include)],
        }},
        'info': {'path': 'Info.plist', 'properties': {
            'CFBundleDisplayName': 'Screen rate', 'UILaunchScreen': {}, 'UIFileSharingEnabled': True,
            'CADisableMinimumFrameDurationOnPhone': True, 'UISupportedInterfaceOrientations': LANDSCAPE,
            'UIApplicationSceneManifest': {'UISceneConfigurations': {'UIWindowSceneSessionRoleApplication': [scene]}},
        }},
    }
    project = {'name': 'ScreenRateCheck', 'options': {'deploymentTarget': {'iOS': '26.0'}}, 'targets': {'ScreenRateCheck': target}}
    spec = root / 'project.yml'
    spec.write_text(json.dumps(project, indent=2) + '\n')
    subprocess.run(['xcodegen', 'generate', '--spec', str(spec)], check=True)
    sdk = 'iphonesimulator' if args.simulator else 'iphoneos'
    subprocess.run(['xcodebuild', '-project', str(root / 'ScreenRateCheck.xcodeproj'), '-scheme', 'ScreenRateCheck', '-configuration', 'Debug',
                    '-destination', 'generic/platform=iOS Simulator' if args.simulator else 'generic/platform=iOS',
                    '-derivedDataPath', str(root / 'DerivedData'), '-jobs', '3', 'CODE_SIGNING_ALLOWED=NO', 'build'], check=True)
    print(root / f'DerivedData/Build/Products/Debug-{sdk}/ScreenRateCheck.app')


if __name__ == '__main__':
    main()
