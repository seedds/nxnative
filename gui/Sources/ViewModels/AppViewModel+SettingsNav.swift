import Foundation
import SwiftUI
import AppKit

extension AppViewModel {
    // MARK: - Gamepad Settings Modal Navigation
    public func handleSettingsModalGamepadAction(_ action: GamepadAction) {
        guard let game = gameForSettingsSheet else { return }
        
        switch action {
        case .navigateUp:
            SoundEffectService.shared.play(.focus)
            withAnimation(.easeInOut(duration: 0.15)) {
                settingsModalFocusIndex = (settingsModalFocusIndex - 1 + 8) % 8
            }
            
        case .navigateDown:
            SoundEffectService.shared.play(.focus)
            withAnimation(.easeInOut(duration: 0.15)) {
                settingsModalFocusIndex = (settingsModalFocusIndex + 1) % 8
            }
            
        case .navigateLeft:
            SoundEffectService.shared.play(.click)
            withAnimation(.easeInOut(duration: 0.15)) {
                switch settingsModalFocusIndex {
                case 0: // 60 FPS
                    activeSettingsConfig.enable60FPS.toggle()
                case 1: // Metal HUD
                    activeSettingsConfig.enableMetalHUD.toggle()
                case 2: // Console Mode
                    activeSettingsConfig.isDockedMode = false
                case 3: // 3D Render Scale (decrement 5%)
                    let current = activeSettingsConfig.renderScale
                    let updated = max(0.50, round((current - 0.05) * 100) / 100)
                    activeSettingsConfig.renderScale = updated
                case 4: // MetalFX
                    activeSettingsConfig.enableMetalFXUpscale.toggle()
                case 5: // Restore Defaults -> Cancel
                    settingsModalFocusIndex = 7
                case 6: // Cancel -> Defaults
                    settingsModalFocusIndex = 5
                case 7: // Save -> Cancel
                    settingsModalFocusIndex = 6
                default:
                    break
                }
            }
            
        case .navigateRight:
            SoundEffectService.shared.play(.click)
            withAnimation(.easeInOut(duration: 0.15)) {
                switch settingsModalFocusIndex {
                case 0: // 60 FPS
                    activeSettingsConfig.enable60FPS.toggle()
                case 1: // Metal HUD
                    activeSettingsConfig.enableMetalHUD.toggle()
                case 2: // Console Mode
                    activeSettingsConfig.isDockedMode = true
                case 3: // 3D Render Scale (increment 5%)
                    let current = activeSettingsConfig.renderScale
                    let updated = min(1.00, round((current + 0.05) * 100) / 100)
                    activeSettingsConfig.renderScale = updated
                case 4: // MetalFX
                    activeSettingsConfig.enableMetalFXUpscale.toggle()
                case 5: // Restore Defaults -> Cancel
                    settingsModalFocusIndex = 6
                case 6: // Cancel -> Save
                    settingsModalFocusIndex = 7
                case 7: // Save -> Restore
                    settingsModalFocusIndex = 5
                default:
                    break
                }
            }
            
        case .confirm: // A / Cross
            SoundEffectService.shared.play(.click)
            withAnimation(.easeInOut(duration: 0.15)) {
                switch settingsModalFocusIndex {
                case 0:
                    activeSettingsConfig.enable60FPS.toggle()
                case 1:
                    activeSettingsConfig.enableMetalHUD.toggle()
                case 2:
                    activeSettingsConfig.isDockedMode.toggle()
                case 3: // Cycle presets (50% -> 75% -> 100% -> 50%)
                    if activeSettingsConfig.renderScale < 0.70 {
                        activeSettingsConfig.renderScale = 0.75
                    } else if activeSettingsConfig.renderScale < 0.95 {
                        activeSettingsConfig.renderScale = 1.00
                    } else {
                        activeSettingsConfig.renderScale = 0.50
                    }
                case 4:
                    activeSettingsConfig.enableMetalFXUpscale.toggle()
                case 5: // Restore Defaults
                    activeSettingsConfig = GameRuntimeConfig()
                case 6: // Cancel
                    gameForSettingsSheet = nil
                case 7: // Save Settings
                    saveRuntimeConfig(activeSettingsConfig, for: game)
                    gameForSettingsSheet = nil
                default:
                    break
                }
            }
            
        case .secondaryAction, .options: // X / Square or Options button: Quick Save & Exit
            SoundEffectService.shared.play(.click)
            saveRuntimeConfig(activeSettingsConfig, for: game)
            gameForSettingsSheet = nil
            
        case .cancel: // B / Circle: Dismiss without saving
            SoundEffectService.shared.play(.click)
            gameForSettingsSheet = nil
            
        case .contextAction: // Y / Triangle: Restore Defaults
            SoundEffectService.shared.play(.click)
            activeSettingsConfig = GameRuntimeConfig()
            
        default:
            break
        }
    }
}
