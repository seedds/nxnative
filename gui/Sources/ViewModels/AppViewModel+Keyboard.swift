import Foundation
import SwiftUI
import AppKit

extension AppViewModel {
    
    // MARK: - Global Keyboard Navigation Handler
    public func handleKeyboardKey(_ key: String) {
        // Modal Navigation: traps all keyboard navigation while any modal is opened
        if isAnyModalPresented {
            if gameForSettingsSheet != nil {
                switch key {
                case "up": handleSettingsModalGamepadAction(.navigateUp)
                case "down": handleSettingsModalGamepadAction(.navigateDown)
                case "left": handleSettingsModalGamepadAction(.navigateLeft)
                case "right": handleSettingsModalGamepadAction(.navigateRight)
                case "return", "space": handleSettingsModalGamepadAction(.confirm)
                case "escape": handleSettingsModalGamepadAction(.cancel)
                case "o": handleSettingsModalGamepadAction(.options)
                case "y": handleSettingsModalGamepadAction(.contextAction)
                default: break
                }
            } else if isGameActionMenuPresented {
                switch key {
                case "up": handleLibraryNavigateUp()
                case "down": handleLibraryNavigateDown()
                case "return", "space": handleConfirm()
                case "escape": handleCancel()
                default: break
                }
            } else if showDeleteConfirm || showRemoveConfirm || showTrashConfirm {
                if key == "escape" {
                    showDeleteConfirm = false
                    showRemoveConfirm = false
                    showTrashConfirm = false
                }
            }
            return
        }
        
        // Tab switching: Direct number keys 1-7
        if let num = Int(key), (1...7).contains(num) {
            SoundEffectService.shared.play(.tabSwitch)
            selectedTab = num - 1
            return
        }
        
        // Tab cycling: Tab key, [ and ]
        if key == "tab" || key == "]" {
            SoundEffectService.shared.play(.tabSwitch)
            selectedTab = (selectedTab + 1) % 7
            return
        }
        if key == "shifttab" || key == "[" {
            SoundEffectService.shared.play(.tabSwitch)
            selectedTab = (selectedTab - 1 + 7) % 7
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
            if selectedTab == 6 {
                handleSecondaryAction()
            }
        case "k":
            if selectedTab == 6 {
                handleContextAction()
            }
        case "i":
            if selectedTab == 5 {
                showInstallerSheet = true
            }
        case "u":
            if selectedTab == 5 {
                checkForUpdates()
            }
        case "space":
            if selectedTab == 3 {
                var prof = InputProfileManager.shared.activeProfile
                prof.swapFaceButtons()
                InputProfileManager.shared.updateProfile(prof)
            }
        case "g":
            if selectedTab == 4 {
                handleSecondaryAction()
            }
        case "m":
            if selectedTab == 4 {
                handleContextAction()
            }
        default:
            break
        }
    }
}
