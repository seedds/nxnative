import XCTest
import UIKit

private struct DuoSnapshot {
    let orientation: Int
    let bounds: [Double]
    let game: [Double]
    let controls: [Double]
    let separated: Bool
    let touchCount: Int
    let touchUpdates: Int
    let lastTouches: [[Double]]
    let hingeStatus: Int
    let hingeAngle: Double

    init?(_ element: XCUIElement) {
        guard let value = element.value as? String,
              let data = value.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let orientation = (object["orientation"] as? NSNumber)?.intValue,
              let bounds = Self.rect(object["bounds"]),
              let game = Self.rect(object["game"]),
              let controls = Self.rect(object["controls"]),
              let separated = object["separated"] as? Bool,
              let touchCount = (object["touchCount"] as? NSNumber)?.intValue,
              let touchUpdates = (object["touchUpdates"] as? NSNumber)?.intValue,
              let lastTouches = object["lastTouches"] as? [[NSNumber]],
              let hingeStatus = (object["hingeStatus"] as? NSNumber)?.intValue,
              let hingeAngle = (object["hingeAngle"] as? NSNumber)?.doubleValue else { return nil }
        self.orientation = orientation
        self.bounds = bounds
        self.game = game
        self.controls = controls
        self.separated = separated
        self.touchCount = touchCount
        self.touchUpdates = touchUpdates
        self.lastTouches = lastTouches.map { $0.map(\.doubleValue) }
        self.hingeStatus = hingeStatus
        self.hingeAngle = hingeAngle
    }

    private static func rect(_ value: Any?) -> [Double]? {
        guard let numbers = value as? [NSNumber], numbers.count == 4 else { return nil }
        return numbers.map(\.doubleValue)
    }
}

final class DuoNativeRotationUITests: XCTestCase {
    @MainActor
    func testObservedNativeFourOrientationCycleOnFullyOpenDuoPreservesLayoutAndTouchRouting() {
        guard #available(iOS 27.1, *) else {
            XCTFail("The native Duo rotation test requires iOS 27.1 hinge and scene-geometry APIs")
            return
        }

        let app = XCUIApplication(bundleIdentifier: "org.nxnative.DuoHarness")
        app.launch()
        defer { app.terminate() }

        let diagnostics = app.descendants(matching: .any).matching(identifier: "duo.diagnostics").firstMatch
        XCTAssertTrue(diagnostics.waitForExistence(timeout: 15), "The harness must expose live scene and input diagnostics")
        guard diagnostics.exists else { return }

        let fullyOpenStatus = UIHinge.Status.fullyOpen.rawValue
        guard let initial = waitForSnapshot(diagnostics, timeout: 15, matching: {
            $0.hingeStatus == fullyOpenStatus && abs($0.hingeAngle - .pi) < 0.12
        }) else {
            XCTFail("Open the Duo fully before running this test. Diagnostics: \(String(describing: diagnostics.value))")
            return
        }
        XCTAssertEqual(initial.hingeStatus, fullyOpenStatus)
        XCTAssertEqual(initial.hingeAngle, .pi, accuracy: 0.12)

        let poses: [(name: String, portrait: Bool)] = [
            ("landscape right", false),
            ("portrait", true),
            ("landscape left", false),
            ("portrait upside down", true)
        ]

        for pose in poses {
            print("[DUO_ROTATION] Waiting for Device Hub to set bounds for (\(pose.name)); rotate within 60 seconds.")
            guard let snapshot = waitForSnapshot(diagnostics, timeout: 60, matching: {
                ($0.bounds[3] > $0.bounds[2]) == pose.portrait
            }) else {
                XCTFail("Native \(pose.name) rotation did not reach the expected scene geometry. Diagnostics: \(String(describing: diagnostics.value))")
                return
            }

            XCTAssertEqual(snapshot.hingeStatus, fullyOpenStatus, "Keep the Duo fully open through the orientation cycle")
            XCTAssertEqual(snapshot.hingeAngle, .pi, accuracy: 0.12)
            XCTAssertGreaterThan(snapshot.bounds[2], 0)
            XCTAssertGreaterThan(snapshot.bounds[3], 0)
            XCTAssertGreaterThan(snapshot.game[2], 0)
            XCTAssertGreaterThan(snapshot.game[3], 0)
            XCTAssertEqual(snapshot.bounds[3] > snapshot.bounds[2], pose.portrait,
                           "Native \(pose.name) rotation must update the actual root view bounds")
            XCTAssertEqual(snapshot.game[2] / snapshot.game[3], 16.0 / 9.0, accuracy: 0.01,
                           "The production game view must keep its 16:9 frame in \(pose.name)")
            XCTAssertGreaterThanOrEqual(snapshot.game[0], snapshot.bounds[0] - 1)
            XCTAssertGreaterThanOrEqual(snapshot.game[1], snapshot.bounds[1] - 1)
            XCTAssertLessThanOrEqual(snapshot.game[0] + snapshot.game[2], snapshot.bounds[0] + snapshot.bounds[2] + 1)
            XCTAssertLessThanOrEqual(snapshot.game[1] + snapshot.game[3], snapshot.bounds[1] + snapshot.bounds[3] + 1)

            if pose.portrait {
                XCTAssertTrue(snapshot.separated, "Both native portrait poses keep the game above the touch controls")
                XCTAssertLessThanOrEqual(snapshot.game[1] + snapshot.game[3], snapshot.controls[1] + 1)
                XCTAssertGreaterThan(snapshot.controls[3], 0)
                assertGameTouch(diagnostics: diagnostics, after: snapshot.touchUpdates)
            } else {
                XCTAssertFalse(snapshot.separated, "The fully-open Duo uses overlay controls in landscape")
                XCTAssertEqual(snapshot.controls[0], snapshot.bounds[0], accuracy: 1)
                XCTAssertEqual(snapshot.controls[1], snapshot.bounds[1], accuracy: 1)
                XCTAssertEqual(snapshot.controls[2], snapshot.bounds[2], accuracy: 1)
                XCTAssertEqual(snapshot.controls[3], snapshot.bounds[3], accuracy: 1)
            }
        }
    }

    @MainActor
    private func assertGameTouch(diagnostics: XCUIElement, after updateCount: Int) {
        print("[DUO_ROTATION] Click the center of the visible Game viewport within 60 seconds.")
        guard let after = waitForSnapshot(diagnostics, timeout: 60, matching: {
            $0.touchUpdates > updateCount && $0.touchCount == 0 && !$0.lastTouches.isEmpty
        }) else {
            XCTFail("A native click in the production game view must reach and release through dm_touch_set")
            return
        }
        XCTAssertEqual(after.touchCount, 0, "The native click must be released after the input update")
        if let touch = after.lastTouches.first, touch.count >= 2 {
            XCTAssertEqual(touch[0], 0.5, accuracy: 0.04)
            XCTAssertEqual(touch[1], 0.5, accuracy: 0.04)
        } else {
            XCTFail("The native click must include a normalized game-view touch sample")
        }
    }

    @MainActor
    private func waitForSnapshot(_ element: XCUIElement, timeout: TimeInterval,
                                matching predicate: @escaping (DuoSnapshot) -> Bool) -> DuoSnapshot? {
        let expectation = XCTNSPredicateExpectation(predicate: NSPredicate { object, _ in
            guard let element = object as? XCUIElement, let snapshot = DuoSnapshot(element) else { return false }
            return predicate(snapshot)
        }, object: element)
        guard XCTWaiter.wait(for: [expectation], timeout: timeout) == .completed else { return nil }
        return DuoSnapshot(element)
    }
}
