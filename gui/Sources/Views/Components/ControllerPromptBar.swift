import SwiftUI

public struct ControllerPromptBar: View {
    @ObservedObject private var controllerService = GameControllerService.shared
    
    public init() {}
    
    public var body: some View {
        HStack(spacing: 16) {
            // Connected status badge
            HStack(spacing: 6) {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 11))
                    .foregroundColor(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.textMuted)
                
                Text(controllerService.isGamepadConnected ? controllerService.activeControllerName : "Gamepad Ready")
                    .font(.system(size: 11, weight: .semibold, design: .rounded))
                    .foregroundColor(controllerService.isGamepadConnected ? NXTheme.textPrimary : NXTheme.textMuted)
                    .lineLimit(1)
            }
            .padding(.horizontal, 9)
            .padding(.vertical, 4)
            .background(
                Capsule()
                    .fill(controllerService.isGamepadConnected ? NXTheme.primary.opacity(0.18) : NXTheme.glassSurfaceElevated.opacity(0.4))
            )
            .overlay(
                Capsule()
                    .strokeBorder(controllerService.isGamepadConnected ? NXTheme.secondary.opacity(0.4) : Color.white.opacity(0.08), lineWidth: 0.8)
            )
            
            Spacer()
            
            // Action button prompts
            HStack(spacing: 14) {
                promptPill(
                    glyph: controllerService.activeBrand == .playstation ? "✕" : "A",
                    action: "Play / Confirm",
                    color: NXTheme.primary
                )
                
                promptPill(
                    glyph: controllerService.activeBrand == .playstation ? "○" : "B",
                    action: "Back",
                    color: Color(red: 0.9, green: 0.3, blue: 0.3)
                )
                
                promptPill(
                    glyph: controllerService.activeBrand == .playstation ? "□" : "X",
                    action: "Settings",
                    color: NXTheme.secondary
                )
                
                promptPill(
                    glyph: controllerService.activeBrand == .playstation ? "△" : "Y",
                    action: "Save Folder",
                    color: Color(red: 0.95, green: 0.75, blue: 0.2)
                )
                
                promptPill(
                    glyph: controllerService.activeBrand.shoulderTabsPrompt,
                    action: "Switch Tabs",
                    color: NXTheme.textSecondary
                )
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(
            ZStack {
                NXTheme.canvasObsidian.opacity(0.75)
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
    
    private func promptPill(glyph: String, action: String, color: Color) -> some View {
        HStack(spacing: 5) {
            Text(glyph)
                .font(.system(size: 10, weight: .black, design: .rounded))
                .foregroundColor(.white)
                .frame(minWidth: 16, minHeight: 16)
                .padding(.horizontal, 3)
                .background(Circle().fill(color.opacity(0.85)))
                .shadow(color: color.opacity(0.4), radius: 3)
            
            Text(action)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)
        }
    }
}
