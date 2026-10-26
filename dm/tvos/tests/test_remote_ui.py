#!/usr/bin/env python3
"""Exercise the production Remote frontend in an isolated, owned TV simulator.

The host functions are stubs; no guest game, resources, physical Apple TV or
existing simulator is modified. Requires Xcode with an available tvOS runtime
and xcodegen. The generated project and xcresult remain in --output.
"""
import argparse
import json
from pathlib import Path
import subprocess
import uuid


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    tv = Path(__file__).resolve().parents[1]
    host = tv.parent / 'host'
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    fixture = out / 'fixture'; fixture.mkdir(exist_ok=True)
    (fixture / 'game_profile.h').write_text('// No game-specific constants are needed by this frontend fixture.\n')
    project = f'''name: TVRemoteHarness
settings:
  CLANG_ENABLE_OBJC_ARC: YES
  GENERATE_INFOPLIST_FILE: YES
  HEADER_SEARCH_PATHS: '"{host}" "{fixture}"'
targets:
  Harness:
    type: application
    platform: tvOS
    deploymentTarget: '17.0'
    sources:
      - path: {tv}/tests/remote_harness/Mock.m
      - path: {tv}/Sources/front_tv.m
    info:
      path: Info.plist
      properties:
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default
                UISceneDelegateClassName: QAScene
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: org.nxnative.tvremote-harness
  HarnessTests:
    type: bundle.ui-testing
    platform: tvOS
    deploymentTarget: '17.0'
    sources:
      - path: {tv}/tests/remote_harness/RemoteUITests.m
    dependencies:
      - target: Harness
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: org.nxnative.tvremote-harness-tests
      TEST_TARGET_NAME: Harness
schemes:
  TVRemoteHarness:
    build:
      targets:
        Harness: all
        HarnessTests: [test]
    test:
      targets:
        - HarnessTests
'''
    (out / 'project.yml').write_text(project)
    run(['xcodegen', 'generate', '--spec', str(out / 'project.yml')])
    runtimes = json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', 'runtimes', '-j']))['runtimes']
    choices = [r for r in runtimes if r.get('isAvailable') and '.tvOS-' in r['identifier']]
    if not choices:
        raise RuntimeError('No available tvOS simulator runtime')
    runtime = max(choices, key=lambda r: tuple(map(int, r['version'].split('.'))))['identifier']
    owned = subprocess.check_output(['xcrun', 'simctl', 'create', 'ZA-Remote-QA-' + uuid.uuid4().hex[:8],
        'com.apple.CoreSimulator.SimDeviceType.Apple-TV-4K-3rd-generation-1080p', runtime], text=True).strip()
    uuid.UUID(owned)
    (out / 'owned-simulator.txt').write_text(owned + '\n')
    try:
        with (out / 'test.log').open('w') as log:
            run(['xcodebuild', '-project', str(out / 'TVRemoteHarness.xcodeproj'), '-scheme', 'TVRemoteHarness',
                '-destination', 'id=' + owned, '-derivedDataPath', str(out / 'Derived'),
                '-resultBundlePath', str(out / 'Remote.xcresult'), 'CODE_SIGNING_ALLOWED=NO', 'test'],
                stdout=log, stderr=subprocess.STDOUT, timeout=600)
    finally:
        # Only the simulator created above is eligible for cleanup.
        subprocess.run(['xcrun', 'simctl', 'shutdown', owned], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['xcrun', 'simctl', 'delete', owned], check=True)
    print('Production Remote UIKit input and focus checks PASS')


if __name__ == '__main__':
    main()
