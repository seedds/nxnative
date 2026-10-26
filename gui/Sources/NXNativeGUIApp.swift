import SwiftUI
import AppKit

@main
struct NXNativeGUIApp: App {
    var body: some Scene {
        WindowGroup {
            MainView()
        }
        .windowStyle(.titleBar)
        .windowToolbarStyle(.unified)
        .defaultSize(width: 820, height: 680)
        .commands {
            SidebarCommands()
        }
    }
}
