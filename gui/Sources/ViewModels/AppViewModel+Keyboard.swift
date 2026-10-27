import Foundation
import SwiftUI
import AppKit

extension AppViewModel {
    
    // MARK: - Global Keyboard Navigation Handler
    public func handleKeyboardKey(_ key: String) {
        // Tab switching: 1-6
        if let num = Int(key), (1...6).contains(num) {
            SoundEffectService.shared.play(.tabSwitch)
            withAnimation(.easeInOut(duration: 0.18)) {
                if isGameActionMenuPresented { isGameActionMenuPresented = false }
                selectedTab = num - 1
            }
            return
        }
        
        switch key {
        case "left":
            handleNavigateLeft()
        case "right":
            handleNavigateRight()
        case "up":
            handleNavigateUp()
        case "down":
            handleNavigateDown()
        case "return":
            handleConfirm()
        case "escape":
            handleCancel()
        case "o":
            handleOptionsAction()
        case ",":
            if selectedTab == 1, let g = currentFocusedGame {
                SoundEffectService.shared.play(.click)
                gameForSettingsSheet = g
            }
        case "r":
            if selectedTab == 2 {
                loadCompatibleGames(forceRefresh: true)
            }
        case "c":
            if selectedTab == 5 {
                handleSecondaryAction()
            }
        case "k":
            if selectedTab == 5 {
                handleContextAction()
            }
        case "i":
            if selectedTab == 4 {
                showInstallerSheet = true
            }
        case "u":
            if selectedTab == 4 {
                checkForUpdates()
            }
        case "space":
            if selectedTab == 3 {
                var prof = InputProfileManager.shared.activeProfile
                prof.swapFaceButtons()
                InputProfileManager.shared.updateProfile(prof)
            }
        default:
            break
        }
    }
}
