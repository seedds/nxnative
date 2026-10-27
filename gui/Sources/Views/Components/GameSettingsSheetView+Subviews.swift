import SwiftUI

// MARK: - Subviews and Card Helpers for GameSettingsSheetView
extension GameSettingsSheetView {
    
    func footerBar(onDismiss: @escaping () -> Void) -> some View {
        VStack(spacing: 10) {
            HStack(spacing: 12) {
                Button("Restore Defaults") {
                    viewModel.activeSettingsConfig = GameRuntimeConfig()
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.regular)
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(viewModel.settingsModalFocusIndex == 5 ? NXTheme.secondary : Color.clear, lineWidth: 2)
                )
                
                Spacer()
                
                Button("Cancel") {
                    viewModel.gameForSettingsSheet = nil
                    onDismiss()
                }
                .buttonStyle(.plain)
                .foregroundColor(viewModel.settingsModalFocusIndex == 6 ? NXTheme.secondary : NXTheme.textSecondary)
                .font(.system(size: 12, weight: viewModel.settingsModalFocusIndex == 6 ? .bold : .regular))
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(viewModel.settingsModalFocusIndex == 6 ? NXTheme.secondary : Color.clear, lineWidth: 1.5)
                )
                
                Button("Save Settings") {
                    viewModel.saveRuntimeConfig(viewModel.activeSettingsConfig, for: game)
                    viewModel.gameForSettingsSheet = nil
                    onDismiss()
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .controlSize(.regular)
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(viewModel.settingsModalFocusIndex == 7 ? Color.white : Color.clear, lineWidth: 2)
                )
                .shadow(color: viewModel.settingsModalFocusIndex == 7 ? NXTheme.primary.opacity(0.8) : Color.clear, radius: 8)
            }
            
            // Controller navigation hints strip
            HStack(spacing: 12) {
                hintBadge(glyph: "↑/↓", text: "Navigate", isPill: true)
                hintBadge(glyph: "←/→", text: "Adjust", isPill: true)
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "✕" : "A",
                    text: "Toggle",
                    bgColor: Color(red: 0.06, green: 0.49, blue: 0.06)
                )
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "□" : "X",
                    text: "Save & Close",
                    bgColor: Color(red: 0.0, green: 0.47, blue: 0.84)
                )
                hintBadge(
                    glyph: controllerService.activeBrand == .playstation ? "○" : "B",
                    text: "Close",
                    bgColor: Color(red: 0.91, green: 0.07, blue: 0.14)
                )
            }
            .padding(.top, 4)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
        .background(
            ZStack {
                NXTheme.glassSurfaceElevated.opacity(0.85)
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
    
    func hintBadge(glyph: String, text: String, isPill: Bool = false, bgColor: Color = Color(white: 0.25)) -> some View {
        HStack(spacing: 4) {
            if isPill {
                Text(glyph)
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(bgColor))
            } else {
                Text(glyph)
                    .font(.system(size: 9, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .frame(width: 16, height: 16)
                    .background(Circle().fill(bgColor))
            }
            Text(text)
                .font(.system(size: 10, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)
        }
    }
    
    func settingCard<Content: View>(
        rowIndex: Int,
        title: String,
        icon: String,
        iconColor: Color,
        @ViewBuilder content: () -> Content
    ) -> some View {
        let isFocused = viewModel.settingsModalFocusIndex == rowIndex
        
        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(iconColor)
                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                
                Spacer()
                
                if isFocused {
                    Text("Focused")
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(NXTheme.primary.opacity(0.3))
                        .clipShape(Capsule())
                }
            }
            
            content()
        }
        .padding(14)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(isFocused ? NXTheme.glassSurfaceElevated.opacity(0.9) : NXTheme.glassSurfaceElevated.opacity(0.65))
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(
                    isFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(NXTheme.specularRim),
                    lineWidth: isFocused ? 2.5 : 0.8
                )
        )
        .shadow(color: isFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 2)
        .animation(.easeInOut(duration: 0.15), value: isFocused)
        .onTapGesture {
            viewModel.settingsModalFocusIndex = rowIndex
        }
    }
}
