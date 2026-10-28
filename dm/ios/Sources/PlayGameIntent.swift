// App Intents: launch this game from Siri, Shortcuts or the Action Button ("Play <game>").
// Opening the app is all the intent does: the game's own launcher and settings then run
// as usual. The phrases use the app's own name, so every game's app has its own.
import AppIntents
import UIKit

struct PlayGameIntent: AppIntent {
    static let title: LocalizedStringResource = "Play"
    static let openAppWhenRun: Bool = true

    func perform() async throws -> some IntentResult {
        .result()
    }
}

struct PlayGameShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: PlayGameIntent(),
            phrases: [
                "Play \(.applicationName)",
                "Start \(.applicationName)",
            ],
            shortTitle: "Play",
            systemImageName: "gamecontroller.fill"
        )
    }
}
