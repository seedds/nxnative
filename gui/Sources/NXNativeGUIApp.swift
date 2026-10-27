import SwiftUI
import AppKit

@main
struct NXNativeGUIApp: App {
    @StateObject private var viewModel = AppViewModel()
    
    var body: some Scene {
        WindowGroup {
            MainView(viewModel: viewModel)
        }
        .windowStyle(.titleBar)
        .windowToolbarStyle(.unified)
        .defaultSize(width: 820, height: 680)
        .commands {
            SidebarCommands()
            
            CommandGroup(replacing: .appInfo) {
                Button("About NxNative Launcher") {
                    SoundEffectService.shared.play(.click)
                    viewModel.showAboutSheet = true
                }
            }
            
            CommandGroup(after: .appInfo) {
                Button("Check for Updates...") {
                    SoundEffectService.shared.play(.click)
                    viewModel.openUpdateCheckerAndAutoInstall()
                }
                .keyboardShortcut("U", modifiers: [.command])
                
                Divider()
            }
        }
    }
}
