import SwiftUI

public struct SystemAudioGamepadCard: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var soundService = SoundEffectService.shared
    @ObservedObject private var controllerService = GameControllerService.shared
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label("Gamepad & Audio Experience", systemImage: "gamecontroller.fill")
                        .font(.system(size: 14, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    Spacer()
                    
                    Text(controllerService.isGamepadConnected ? "Controller Connected" : "No Controller Detected")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(controllerService.isGamepadConnected ? Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.35) : NXTheme.glassSurfaceElevated)
                        .foregroundColor(.white)
                        .clipShape(Capsule())
                }
                
                Divider().opacity(0.2)
                
                // Gamepad Info Row
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Active Gamepad Device")
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundColor(NXTheme.textPrimary)
                        Text(controllerService.activeControllerName)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                    
                    Button("View Button Layout") {
                        SoundEffectService.shared.play(.click)
                        viewModel.selectedTab = 3 // Controls tab
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                }
                
                Divider().opacity(0.15)
                
                // Audio Toggle Row
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Console UI Sound Effects")
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundColor(NXTheme.textPrimary)
                        Text("Play mechanical feedback tones when clicking buttons, navigating tabs, and launching games.")
                            .font(.system(size: 10))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                    
                    Toggle("", isOn: $soundService.isSoundEnabled)
                        .toggleStyle(.switch)
                        .labelsHidden()
                        .onChange(of: soundService.isSoundEnabled) { val in
                            if val {
                                SoundEffectService.shared.play(.click)
                            }
                        }
                }
            }
        }
    }
}
