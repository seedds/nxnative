import Foundation
import SwiftUI
import AppKit

extension AppViewModel {
    
    public func currentFilteredCompatibleGames() -> [CompatibleGameItem] {
        var list = compatibleGames
        let query = compatibilitySearchText.trimmingCharacters(in: .whitespaces).lowercased()
        if !query.isEmpty {
            list = list.filter {
                $0.title.lowercased().contains(query) ||
                $0.titleId.lowercased().contains(query) ||
                $0.engine.lowercased().contains(query) ||
                $0.graphicsApi.lowercased().contains(query)
            }
        }
        
        switch compatibilityFilter {
        case "Unreal":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("unreal") }
        case "Unity":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("unity") }
        case "Nintendo":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("nintendo") || $0.engine.localizedCaseInsensitiveContains("sead") }
        case "RE Engine":
            list = list.filter { $0.engine.localizedCaseInsensitiveContains("re engine") || $0.engine.localizedCaseInsensitiveContains("capcom") }
        default:
            break
        }
        return list
    }
    
    public func handleCompatibilityNavigateLeft() {
        if compatibilityFocusArea == .filters {
            cycleCompatibilityFilter(forward: false)
            SoundEffectService.shared.play(.focus)
        } else {
            if compatibilityDisplayMode == .list {
                focusedCompatibilityButtonIndex = .notes
                SoundEffectService.shared.play(.focus)
            } else {
                if focusedCompatibilityIndex > 0 {
                    focusedCompatibilityIndex -= 1
                    SoundEffectService.shared.play(.focus)
                }
            }
        }
    }
    
    public func handleCompatibilityNavigateRight() {
        if compatibilityFocusArea == .filters {
            cycleCompatibilityFilter(forward: true)
            SoundEffectService.shared.play(.focus)
        } else {
            let games = currentFilteredCompatibleGames()
            if focusedCompatibilityIndex < games.count - 1 {
                focusedCompatibilityIndex += 1
                SoundEffectService.shared.play(.focus)
            }
        }
    }
    
    public func handleCompatibilityNavigateUp() {
        if compatibilityFocusArea == .games {
            if compatibilityDisplayMode == .list {
                if focusedCompatibilityIndex > 0 {
                    focusedCompatibilityIndex -= 1
                    SoundEffectService.shared.play(.focus)
                } else {
                    compatibilityFocusArea = .filters
                    SoundEffectService.shared.play(.focus)
                }
            } else {
                let columns = 4
                if focusedCompatibilityIndex >= columns {
                    focusedCompatibilityIndex -= columns
                    SoundEffectService.shared.play(.focus)
                } else {
                    compatibilityFocusArea = .filters
                    SoundEffectService.shared.play(.focus)
                }
            }
        }
    }
    
    public func handleCompatibilityNavigateDown() {
        let games = currentFilteredCompatibleGames()
        guard !games.isEmpty else { return }
        
        if compatibilityFocusArea == .filters {
            compatibilityFocusArea = .games
            focusedCompatibilityIndex = min(focusedCompatibilityIndex, games.count - 1)
            SoundEffectService.shared.play(.focus)
        } else {
            if compatibilityDisplayMode == .list {
                if focusedCompatibilityIndex < games.count - 1 {
                    focusedCompatibilityIndex += 1
                    SoundEffectService.shared.play(.focus)
                }
            } else {
                let columns = 4
                if focusedCompatibilityIndex + columns < games.count {
                    focusedCompatibilityIndex += columns
                    SoundEffectService.shared.play(.focus)
                } else if focusedCompatibilityIndex < games.count - 1 {
                    focusedCompatibilityIndex = games.count - 1
                    SoundEffectService.shared.play(.focus)
                }
            }
        }
    }
    
    public func handleCompatibilityConfirm() {
        let games = currentFilteredCompatibleGames()
        if compatibilityFocusArea == .filters {
            if !games.isEmpty {
                compatibilityFocusArea = .games
                focusedCompatibilityIndex = 0
                SoundEffectService.shared.play(.click)
            }
        } else {
            guard focusedCompatibilityIndex >= 0 && focusedCompatibilityIndex < games.count else { return }
            let targetGame = games[focusedCompatibilityIndex]
            
            SoundEffectService.shared.play(.click)
            if let url = targetGame.githubUrl {
                NSWorkspace.shared.open(url)
            }
        }
    }
    
    public func handleCompatibilityCancel() {
        if compatibilityFocusArea == .games {
            SoundEffectService.shared.play(.click)
            compatibilityFocusArea = .filters
        }
    }
    
    public func handleCompatibilitySecondaryAction() {
        let games = currentFilteredCompatibleGames()
        guard focusedCompatibilityIndex >= 0 && focusedCompatibilityIndex < games.count else { return }
        let targetGame = games[focusedCompatibilityIndex]
        if let url = targetGame.githubUrl {
            SoundEffectService.shared.play(.click)
            NSWorkspace.shared.open(url)
        }
    }
}
