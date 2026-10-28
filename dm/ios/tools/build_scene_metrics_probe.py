#!/usr/bin/env python3
"""Build an unsigned native iOS scene-metrics probe with synthetic host stubs.

GAME selects only source profile constants. No game files, keys or signing are
used. Install/run the resulting app separately on an authorized test device.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new scratch directory')
    args = parser.parse_args()
    game = os.environ.get('GAME') or os.environ.get('DM_GAME')
    if not game:
        parser.error('GAME or DM_GAME must select a source profile')
    output = args.output.resolve()
    output.mkdir(parents=True)
    include = output / 'Include'
    include.mkdir()
    subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'header', game,
                    str(include / 'game_profile.h')], check=True)
    project = {
        'name': 'DisplayVerification',
        'options': {'deploymentTarget': {'iOS': '26.0'}},
        'targets': {'DisplayVerification': {
            'type': 'application', 'platform': 'iOS',
            'sources': [{'path': str(REPO / 'dm/ios/tests/scene_metrics_check.m')}],
            'dependencies': [{'sdk': name + '.framework'} for name in
                             ('UIKit', 'Foundation', 'QuartzCore', 'Metal', 'AVFAudio', 'GameController')],
            'settings': {'base': {
                'PRODUCT_BUNDLE_IDENTIFIER': 'dev.anygame.display-verification',
                'ENABLE_DEBUG_DYLIB': 'NO', 'CODE_SIGN_STYLE': 'Automatic',
                'TARGETED_DEVICE_FAMILY': '1',
                'HEADER_SEARCH_PATHS': [str(REPO / 'dm/host'), str(include)],
            }},
            'info': {'path': 'Info.plist', 'properties': {
                'CFBundleDisplayName': 'Display Verification', 'UILaunchScreen': {},
                'UIFileSharingEnabled': True,
                'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
                'UIApplicationSceneManifest': {'UIApplicationSupportsMultipleScenes': False,
                    'UISceneConfigurations': {'UIWindowSceneSessionRoleApplication': [{
                        'UISceneConfigurationName': 'Default', 'UISceneDelegateClassName': 'MetricsScene'}]}},
            }},
        }},
    }
    spec = output / 'project.yml'
    spec.write_text(json.dumps(project, indent=2) + '\n')
    subprocess.run(['xcodegen', 'generate', '--spec', str(spec)], check=True)
    subprocess.run(['xcodebuild', '-project', str(output / 'DisplayVerification.xcodeproj'),
                    '-scheme', 'DisplayVerification', '-configuration', 'Debug',
                    '-destination', 'generic/platform=iOS', '-derivedDataPath', str(output / 'DerivedData'),
                    '-jobs', '3', 'CODE_SIGNING_ALLOWED=NO', 'build'], check=True)
    print(output / 'DerivedData/Build/Products/Debug-iphoneos/DisplayVerification.app')


if __name__ == '__main__':
    main()
