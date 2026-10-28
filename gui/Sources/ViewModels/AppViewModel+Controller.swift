import Foundation
import SwiftUI
import AppKit

extension AppViewModel {
    public func setupControllerNavigation() {
        GameControllerService.shared.onAction = { [weak self] action in
            self?.handleGamepadAction(action)
        }
    }
    
    public func handleGamepadAction(_ action: GamepadAction) {
        if isAnyModalPresented {
            if gameForSettingsSheet != nil {
                handleSettingsModalGamepadAction(action)
            } else if isGameActionMenuPresented {
                switch action {
                case .navigateUp: handleLibraryNavigateUp()
                case .navigateDown: handleLibraryNavigateDown()
                case .confirm: handleConfirm()
                case .cancel: handleCancel()
                default: break
                }
            }
            return
        }
        
        switch action {
        case .tabPrevious:
            SoundEffectService.shared.play(.tabSwitch)
            withAnimation(.easeInOut(duration: 0.18)) {
                selectedTab = (selectedTab - 1 + 7) % 7
            }
            
        case .tabNext:
            SoundEffectService.shared.play(.tabSwitch)
            withAnimation(.easeInOut(duration: 0.18)) {
                selectedTab = (selectedTab + 1) % 7
            }
            
        case .navigateLeft:
            handleNavigateLeft()
            
        case .navigateRight:
            handleNavigateRight()
            
        case .navigateDown:
            handleNavigateDown()
            
        case .navigateUp:
            handleNavigateUp()
            
        case .confirm:
            handleConfirm()
        case .cancel:
            handleCancel()
        case .secondaryAction:
            handleSecondaryAction()
        case .contextAction:
            handleContextAction()
        case .menu:
            SoundEffectService.shared.play(.click)
            if selectedTab == 1, let game = currentFocusedGame {
                gameForSettingsSheet = game
            } else {
                selectedTab = 5 // Settings
            }
        case .options:
            handleOptionsAction()
        }
    }
    
    // MARK: - Navigation Handlers Across Tabs
    func handleNavigateLeft() {
        switch selectedTab {
        case 0 where buildWizardStep > 1:
            SoundEffectService.shared.play(.focus)
            withAnimation(.easeInOut(duration: 0.2)) { goToWizardStep(buildWizardStep - 1) }
        case 1:
            handleLibraryNavigateLeft()
        case 2:
            handleCompatibilityNavigateLeft()
        case 3:
            let profiles = InputProfileManager.shared.profiles
            if let idx = profiles.firstIndex(where: { $0.id == InputProfileManager.shared.activeProfileId }), idx > 0 {
                InputProfileManager.shared.activeProfileId = profiles[idx - 1].id
                SoundEffectService.shared.play(.focus)
            }
        case 4:
            let themes = LauncherHighlightTheme.allCases
            if let idx = themes.firstIndex(of: highlightTheme), idx > 0 {
                highlightTheme = themes[idx - 1]
                SoundEffectService.shared.play(.focus)
            }
        default: break
        }
    }
    
    func handleNavigateRight() {
        switch selectedTab {
        case 0 where buildWizardStep == 1 && selectedGame != nil:
            SoundEffectService.shared.play(.focus)
            withAnimation(.easeInOut(duration: 0.2)) { goToWizardStep(2) }
        case 1:
            handleLibraryNavigateRight()
        case 2:
            handleCompatibilityNavigateRight()
        case 3:
            let profiles = InputProfileManager.shared.profiles
            if let idx = profiles.firstIndex(where: { $0.id == InputProfileManager.shared.activeProfileId }), idx < profiles.count - 1 {
                InputProfileManager.shared.activeProfileId = profiles[idx + 1].id
                SoundEffectService.shared.play(.focus)
            }
        case 4:
            let themes = LauncherHighlightTheme.allCases
            if let idx = themes.firstIndex(of: highlightTheme), idx < themes.count - 1 {
                highlightTheme = themes[idx + 1]
                SoundEffectService.shared.play(.focus)
            }
        default: break
        }
    }
    
    func handleNavigateUp() {
        switch selectedTab {
        case 0 where buildWizardStep == 1 && !detectedGames.isEmpty:
            if let cur = selectedGame, let idx = detectedGames.firstIndex(of: cur), idx > 0 {
                selectedGame = detectedGames[idx - 1]
                SoundEffectService.shared.play(.focus)
            }
        case 1:
            handleLibraryNavigateUp()
        case 2:
            handleCompatibilityNavigateUp()
        case 5:
            refreshPrerequisites()
        default: break
        }
    }
    
    func handleNavigateDown() {
        switch selectedTab {
        case 0 where buildWizardStep == 1 && !detectedGames.isEmpty:
            if let cur = selectedGame, let idx = detectedGames.firstIndex(of: cur), idx < detectedGames.count - 1 {
                selectedGame = detectedGames[idx + 1]
                SoundEffectService.shared.play(.focus)
            } else if selectedGame == nil {
                selectedGame = detectedGames.first
                SoundEffectService.shared.play(.focus)
            }
        case 1:
            handleLibraryNavigateDown()
        case 2:
            handleCompatibilityNavigateDown()
        default: break
        }
    }
    
    func handleConfirm() {
        SoundEffectService.shared.play(.click)
        switch selectedTab {
        case 0:
            if buildWizardStep == 1 {
                if selectedGame != nil {
                    withAnimation(.easeInOut(duration: 0.2)) { goToWizardStep(2) }
                } else if let first = detectedGames.first {
                    selectedGame = first
                }
            } else if buildWizardStep == 2 && canStartBuild {
                withAnimation(.easeInOut(duration: 0.2)) { startBuild() }
            }
        case 1:
            if isGameActionMenuPresented {
                executeFocusedGameAction()
            } else if let game = currentFocusedGame {
                launchGame(game)
            }
        case 2:
            handleCompatibilityConfirm()
        case 3:
            var prof = InputProfileManager.shared.activeProfile
            prof.swapFaceButtons()
            InputProfileManager.shared.updateProfile(prof)
        case 5:
            if !prerequisites.allSatisfied { showInstallerSheet = true }
        default:
            break
        }
    }
    
    func handleCancel() {
        if gameForSettingsSheet != nil {
            gameForSettingsSheet = nil
            SoundEffectService.shared.play(.click)
            return
        }
        switch selectedTab {
        case 0:
            if buildWizardStep > 1 {
                SoundEffectService.shared.play(.click)
                withAnimation(.easeInOut(duration: 0.2)) { goToWizardStep(buildWizardStep - 1) }
            }
        case 1:
            if isGameActionMenuPresented {
                SoundEffectService.shared.play(.click)
                withAnimation(.easeInOut(duration: 0.18)) { isGameActionMenuPresented = false }
            } else if isFocusOnDeck {
                isFocusOnDeck = false
                SoundEffectService.shared.play(.click)
            }
        case 2:
            handleCompatibilityCancel()
        default:
            break
        }
    }
    
    func handleSecondaryAction() {
        switch selectedTab {
        case 1:
            if let game = currentFocusedGame { launchGame(game) }
        case 2:
            handleCompatibilitySecondaryAction()
        case 6:
            let allText = logs.map { "\($0.timestamp) [\($0.level.rawValue.uppercased())] \($0.message)" }.joined(separator: "\n")
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(allText, forType: .string)
        default:
            break
        }
    }
    
    func handleContextAction() {
        switch selectedTab {
        case 1:
            if let game = currentFocusedGame {
                SoundEffectService.shared.play(.click)
                let profile = GameDetailsProfile.inspect(for: game)
                revealSaveFolder(profile.saveDirectory)
            }
        case 6:
            SoundEffectService.shared.play(.click)
            logs.removeAll()
        default:
            break
        }
    }
    
    // MARK: - Game Action Menu Execution
    public func executeFocusedGameAction() {
        guard let game = currentFocusedGame else { return }
        isGameActionMenuPresented = false
        switch gameActionFocusedIndex {
        case 0:
            launchGame(game)
        case 1:
            gameForSettingsSheet = game
        case 2, 4:
            revealInFinder(url: game.url)
        case 3:
            let profile = GameDetailsProfile.inspect(for: game)
            revealSaveFolder(profile.saveDirectory)
        case 5:
            gameToDelete = game
            showDeleteConfirm = true
        default:
            break
        }
    }
    
    public var currentFocusedGame: PortedGame? {
        guard !libraryGames.isEmpty else { return nil }
        let clamped = max(0, min(focusedGameIndex, libraryGames.count - 1))
        return libraryGames[clamped]
    }
    func handleOptionsAction() {
        if gameForSettingsSheet != nil {
            if let g = gameForSettingsSheet { saveRuntimeConfig(activeSettingsConfig, for: g) }
            gameForSettingsSheet = nil
            SoundEffectService.shared.play(.click)
            return
        }
        if selectedTab == 1 && currentFocusedGame != nil {
            SoundEffectService.shared.play(.click)
            withAnimation(.easeInOut(duration: 0.18)) {
                isGameActionMenuPresented.toggle()
                if isGameActionMenuPresented { gameActionFocusedIndex = 0 }
            }
        }
    }
}
