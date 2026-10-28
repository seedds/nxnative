import SwiftUI

public struct ControllerLiveStatusCard: View {
    @ObservedObject private var controllerService = GameControllerService.shared
    
    public init() {}
    
    public var body: some View {
        GlassCard(padding: 14) {
            HStack(spacing: 14) {
                ZStack {
                    Circle()
                        .fill(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.2) : NXTheme.glassSurfaceElevated)
                        .frame(width: 42, height: 42)
                    
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 20))
                        .foregroundColor(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.textMuted)
                }
                
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 8) {
                        Text(controllerService.isGamepadConnected ? controllerService.activeControllerName : L10n.t("controls.gamepad_none"))
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        Text(controllerService.activeBrand.rawValue)
                            .font(.system(size: 9, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(NXTheme.primary.opacity(0.25))
                            .foregroundColor(NXTheme.secondary)
                            .clipShape(Capsule())
                    }
                    
                    Text(controllerService.isGamepadConnected ? L10n.t("controls.status_connected_desc") : L10n.t("controls.status_disconnected_desc"))
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                }
                
                Spacer()
                
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    controllerService.refreshControllers()
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.clockwise")
                        Text(L10n.t("controls.detect"))
                    }
                    .font(.system(size: 11, weight: .semibold))
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }
        }
    }
}
