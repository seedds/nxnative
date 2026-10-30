# Duo UI and input harness

This XCTest app compiles the production iOS gamepad, layout and hosting-controller code with inert host-runtime stubs. Its fixture `game_profile.h` supplies no game data, so the harness does not generate or require a game profile or extracted files.

The Simulator unit tests cover portrait and landscape layout, safe areas, valid and invalid division regions, 16:9 fitting, touch hit ownership, multitouch and video coordinates, pose-change input release, controller-to-player slots, stale controller callbacks, and preservation of a seeded controller-motion sample across layout changes. The optional virtual-controller case runs when Simulator exposes one.

The native rotation UI test requires the iOS 27.1 iPhone Duo simulator with the hinge fully open. The simulator does not apply scene rotations from `XCUIDevice.shared.orientation` in this test path, so the test waits for an operator to rotate the simulator through Device Hub. It verifies the live bounds and video/control frames while the operator cycles through both landscape and both portrait poses. Inner-display composition follows bounds rather than the scene orientation value. The phone plist deliberately permits only landscape, while the iPad list permits all four orientations. In each portrait pose, click the center of the visible Game viewport within 60 seconds; the test verifies that the native click reaches `dm_touch_set` with a centered normalized game coordinate. It fails if the hinge is not fully open, any pose is not reached, or either native click is missing. The Simulator unit tests continue to use synthetic touches, controller snapshots, hinge statuses, and division callbacks; they do not exercise UIKit hit-testing or system touch cancellation.

The harness uses synthetic host state and does not validate an installed game, physical hinge sensors or device motion.

From the repository root:

```sh
cd dm/ios/tests/duo
xcodegen generate --spec project.yml
xcodebuild test -project DuoHarness.xcodeproj -scheme DuoHarness \
  -destination 'platform=iOS Simulator,id=<simulator-udid>' \
  -parallel-testing-enabled NO CODE_SIGNING_ALLOWED=NO
```

Run the native rotation test on the fully-open iPhone Duo simulator. While the test waits for each pose, rotate the simulator through the printed Device Hub sequence before its 60-second timeout. At each portrait prompt, click the visible Game viewport center before continuing:

```sh
xcodebuild test -project DuoHarness.xcodeproj -scheme DuoHarnessNativeRotation \
  -destination 'platform=iOS Simulator,id=<iphone-duo-simulator-udid>' \
  -parallel-testing-enabled NO \
  CODE_SIGNING_ALLOWED=NO \
  -only-testing:DuoHarnessUITests/DuoNativeRotationUITests/testObservedNativeFourOrientationCycleOnFullyOpenDuoPreservesLayoutAndTouchRouting
```

To build and install the harness app for a simulator:

```sh
DERIVED_DATA="$(mktemp -d "${TMPDIR:-/tmp}/duo-harness.XXXXXX")"
xcodebuild build-for-testing -project DuoHarness.xcodeproj -scheme DuoHarness \
  -destination 'generic/platform=iOS Simulator' -derivedDataPath "$DERIVED_DATA" \
  CODE_SIGNING_ALLOWED=NO
DEVICE=<simulator-udid>
APP="$DERIVED_DATA/Build/Products/Debug-iphonesimulator/DuoHarness.app"
xcrun simctl install "$DEVICE" "$APP"
xcrun simctl launch "$DEVICE" org.nxnative.DuoHarness
```
