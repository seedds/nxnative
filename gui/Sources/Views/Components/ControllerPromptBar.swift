import SwiftUI

public struct ControllerPromptBar: View {
    @ObservedObject var controllerService = GameControllerService.shared
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.nxLiquidGlass) private var glassEnv
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var useFloatingGlassDock: Bool {
        glassEnv.isSupportedAndEnabled && !reduceTransparency
    }
    
    public var body: some View {
        NXGlassContainer(spacing: 10) {
            HStack(spacing: 16) {
                // Connected status badge (Gamepad or Keyboard)
                statusBadge
                
                Spacer()
                
                // Dynamic context-sensitive Action button prompts
                if controllerService.isGamepadConnected {
                    HStack(spacing: 14) {
                        controllerPromptsForCurrentContext
                        
                        // Shoulder bumper prompt for Tabs
                        shoulderTabsPromptView
                    }
                    .transaction { $0.animation = nil }
                } else {
                    // Keyboard shortcuts when no gamepad is connected
                    HStack(spacing: 12) {
                        keyboardPromptsForCurrentContext
                        
                        keyboardKeyBadge(glyph: "1-7", action: viewModel.tr("controls.tabs"))
                    }
                    .transaction { $0.animation = nil }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .modifier(PromptBarDockModifier(useFloatingGlassDock: useFloatingGlassDock))
        }
        .padding(.horizontal, useFloatingGlassDock ? 14 : 0)
        .padding(.bottom, useFloatingGlassDock ? 8 : 0)
        .padding(.top, useFloatingGlassDock ? 2 : 0)
    }
    
    // MARK: - Status Badge
    private var statusBadge: some View {
        HStack(spacing: 6) {
            Image(systemName: controllerService.isGamepadConnected ? "gamecontroller.fill" : "keyboard.fill")
                .font(.system(size: 11))
                .foregroundColor(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.secondary)
            
            Text(controllerService.isGamepadConnected ? controllerService.activeControllerName : viewModel.tr("controls.keyboard_controls"))
                .font(.system(size: 11, weight: .semibold, design: .rounded))
                .foregroundColor(controllerService.isGamepadConnected ? NXTheme.textPrimary : NXTheme.textSecondary)
                .lineLimit(1)
        }
        .padding(.horizontal, 9)
        .padding(.vertical, 4)
        .background(
            Capsule()
                .fill(
                    controllerService.isGamepadConnected
                        ? NXTheme.primary.opacity(0.24)
                        : Color.white.opacity(useFloatingGlassDock ? 0.08 : 0.05)
                )
        )
        .overlay(
            Capsule()
                .strokeBorder(
                    controllerService.isGamepadConnected
                        ? NXTheme.secondary.opacity(0.45)
                        : Color.white.opacity(0.14),
                    lineWidth: 0.8
                )
        )
    }
    
    // MARK: - Controller Prompts
    @ViewBuilder
    private var controllerPromptsForCurrentContext: some View {
        let isPS = controllerService.activeBrand == .playstation
        let confirmBtn = isPS ? "✕" : "A"
        let cancelBtn = isPS ? "○" : "B"
        let xBtn = isPS ? "□" : "X"
        let yBtn = isPS ? "△" : "Y"
        let optionsBtn = controllerService.activeBrand == .xbox ? "⧉" : (isPS ? "OPT" : "−")
        
        switch viewModel.selectedTab {
        case 0: // Port Game Wizard
            controllerPill(glyph: confirmBtn, action: viewModel.buildWizardStep == 1 ? "Select / Next" : (viewModel.buildWizardStep == 2 ? "Start Porting" : "Status"), color: brandAColor)
            if viewModel.buildWizardStep > 1 {
                controllerPill(glyph: cancelBtn, action: "Prev Step", color: brandBColor)
            }
            if viewModel.buildWizardStep == 1 && !viewModel.detectedGames.isEmpty {
                dpadPill(glyph: "↑/↓", action: "Choose Game")
            }
            
        case 1: // Library
            if viewModel.isGameActionMenuPresented {
                controllerPill(glyph: confirmBtn, action: "Execute", color: brandAColor)
                controllerPill(glyph: cancelBtn, action: "Close", color: brandBColor)
                dpadPill(glyph: "↑/↓", action: "Navigate")
            } else {
                let isIOS = viewModel.currentFocusedGame?.platform == .ios
                if isIOS {
                    controllerPill(glyph: confirmBtn, action: "Install / Options", color: brandAColor)
                } else {
                    playPromptPill(aGlyph: confirmBtn, xGlyph: xBtn)
                }
                optionsPill(glyph: optionsBtn, action: "Game Options")
                dpadPill(glyph: "←/→", action: "Browse")
                controllerPill(glyph: yBtn, action: "Saves", color: brandYColor)
            }
            
        case 2: // Compatibility
            if viewModel.compatibilityFocusArea == .filters {
                dpadPill(glyph: "←/→", action: "Filter")
                dpadPill(glyph: "↓", action: "Games")
                controllerPill(glyph: confirmBtn, action: "Select", color: brandAColor)
            } else {
                dpadPill(glyph: "↑/↓", action: "Browse")
                controllerPill(glyph: confirmBtn, action: "View Notes", color: brandAColor)
                controllerPill(glyph: cancelBtn, action: "Filters", color: brandBColor)
            }
            
        case 3: // Controls
            controllerPill(glyph: confirmBtn, action: "Calibrate", color: brandAColor)
            
        case 4: // Personalize
            dpadPill(glyph: "←/→", action: "Highlight Color")
            controllerPill(
                glyph: xBtn,
                action: viewModel.tr(viewModel.isLiquidGlassEnabled ? "personalization.liquid_glass_on" : "personalization.liquid_glass_off"),
                color: brandXColor
            )
            controllerPill(
                glyph: yBtn,
                action: viewModel.tr(viewModel.liquidGlassAppearance.titleLocalizationKey),
                color: brandYColor
            )
            
        case 5: // Settings
            controllerPill(glyph: confirmBtn, action: "Install Tools", color: brandAColor)
            controllerPill(glyph: xBtn, action: "Check Updates", color: brandXColor)
            
        case 6: // Console / Logs
            controllerPill(glyph: xBtn, action: "Copy", color: brandXColor)
            controllerPill(glyph: yBtn, action: "Clear", color: brandBColor)
            
        default:
            EmptyView()
        }
    }
    
    // MARK: - Keyboard Prompts (When no controller is connected)
    @ViewBuilder
    private var keyboardPromptsForCurrentContext: some View {
        switch viewModel.selectedTab {
        case 0: // Port Game
            keyboardKeyBadge(glyph: "↑/↓", action: "Choose")
            keyboardKeyBadge(glyph: "↵ Enter", action: "Next / Port")
            keyboardKeyBadge(glyph: "⎋ Esc", action: "Back")
            
        case 1: // Library
            if viewModel.isGameActionMenuPresented {
                keyboardKeyBadge(glyph: "↑/↓", action: "Navigate")
                keyboardKeyBadge(glyph: "↵ Enter", action: "Execute")
                keyboardKeyBadge(glyph: "⎋ Esc", action: "Close")
            } else {
                let isIOS = viewModel.currentFocusedGame?.platform == .ios
                keyboardKeyBadge(glyph: "← / →", action: "Browse")
                keyboardKeyBadge(glyph: "↵ Enter", action: isIOS ? "Install / Options" : "Play")
                keyboardKeyBadge(glyph: "O", action: "Options")
                keyboardKeyBadge(glyph: ",", action: "Settings")
            }
            
        case 2: // Compatibility
            if viewModel.compatibilityFocusArea == .filters {
                keyboardKeyBadge(glyph: "← / →", action: "Filter")
                keyboardKeyBadge(glyph: "↓", action: "Browse Games")
            } else {
                keyboardKeyBadge(glyph: "↑/↓", action: "Browse")
                keyboardKeyBadge(glyph: "↵ Enter", action: "View Notes")
                keyboardKeyBadge(glyph: "⎋ Esc", action: "Filters")
            }
            keyboardKeyBadge(glyph: "R", action: "Refresh")
            
        case 3: // Controls
            keyboardKeyBadge(glyph: "Space", action: "Calibrate")
            
        case 4: // Personalize
            keyboardKeyBadge(glyph: "← / →", action: "Highlight Color")
            keyboardKeyBadge(
                glyph: "G",
                action: viewModel.tr(viewModel.isLiquidGlassEnabled ? "personalization.liquid_glass_on" : "personalization.liquid_glass_off")
            )
            keyboardKeyBadge(
                glyph: "M",
                action: viewModel.tr(viewModel.liquidGlassAppearance.titleLocalizationKey)
            )
            
        case 5: // Settings
            keyboardKeyBadge(glyph: "I", action: "Install Tools")
            keyboardKeyBadge(glyph: "U", action: "Check Updates")
            
        case 6: // Console
            keyboardKeyBadge(glyph: "C", action: "Copy Logs")
            keyboardKeyBadge(glyph: "K", action: "Clear")
            
        default:
            EmptyView()
        }
    }
}

private struct PromptBarDockModifier: ViewModifier {
    let useFloatingGlassDock: Bool
    
    func body(content: Content) -> some View {
        if useFloatingGlassDock {
            content.nxGlassBar(in: Capsule())
        } else {
            content
                .background(
                    ZStack {
                        NXTheme.canvasObsidian.opacity(0.85)
                        Rectangle().fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle),
                    alignment: .top
                )
        }
    }
}

