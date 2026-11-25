import XCTest
@testable import DuoHarness

final class DuoGameMenuTests: XCTestCase {
    func testMenuLabelsAcceptLaunchAndEnvFilePayloads() {
        let words = ["Plus de réglages", "Disposition de la manette", "Tactile", "Défini dans env.txt"]
        for separator in ["\n", "\\n"] {
            let payload = words.joined(separator: separator)
            XCTAssertEqual(GameMenuText.allCases.map { GameMenuText.line($0, in: payload) }, words)
        }
    }

    func testMissingMenuLabelsKeepTheirEnglishFallbacks() {
        let defaults = ["More settings on the start screen", "Controller layout", "Tap", "Set in env.txt"]
        XCTAssertEqual(GameMenuText.allCases.map { GameMenuText.line($0, in: nil) }, defaults)
        let partial = "Plus de réglages\n \t\nTactile"
        XCTAssertEqual(GameMenuText.allCases.map { GameMenuText.line($0, in: partial) },
                       ["Plus de réglages", "Controller layout", "Tactile", "Set in env.txt"])
        let legacy = "Plus de réglages\\nDisposition de la manette"
        XCTAssertEqual(GameMenuText.allCases.map { GameMenuText.line($0, in: legacy) },
                       ["Plus de réglages", "Disposition de la manette", "Tap", "Set in env.txt"])
    }
}
