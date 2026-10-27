import SwiftUI

// MARK: - Row Subviews and Helpers for CompactConsoleActionSheetView
extension CompactConsoleActionSheetView {
    
    func badgePill(text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .semibold, design: .rounded))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(NXTheme.glassSurfaceElevated.opacity(0.8))
            .foregroundColor(NXTheme.textSecondary)
            .clipShape(Capsule())
    }
    
    func menuActionRow(
        index: Int,
        title: String,
        subtitle: String,
        icon: String,
        isPrimary: Bool,
        isDestructive: Bool = false,
        isLoading: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        let isFocused = viewModel.gameActionFocusedIndex == index
        
        return Button(action: {
            SoundEffectService.shared.play(.click)
            viewModel.gameActionFocusedIndex = index
            action()
        }) {
            HStack(spacing: 14) {
                // Icon Box
                ZStack {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .fill(
                            isDestructive
                                ? Color(red: 0.9, green: 0.25, blue: 0.25).opacity(isFocused ? 0.35 : 0.15)
                                : (isPrimary ? NXTheme.primary.opacity(isFocused ? 0.4 : 0.2) : NXTheme.glassSurfaceElevated)
                        )
                        .frame(width: 34, height: 34)
                    
                    if isLoading {
                        ProgressView()
                            .controlSize(.small)
                    } else {
                        Image(systemName: icon)
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(
                                isDestructive
                                    ? Color(red: 0.95, green: 0.35, blue: 0.35)
                                    : (isPrimary ? NXTheme.secondary : NXTheme.textSecondary)
                            )
                    }
                }
                
                // Labels
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(
                            isDestructive
                                ? Color(red: 0.95, green: 0.4, blue: 0.4)
                                : (isFocused ? NXTheme.textPrimary : NXTheme.textPrimary.opacity(0.9))
                        )
                    
                    Text(subtitle)
                        .font(.system(size: 10, design: .rounded))
                        .foregroundColor(NXTheme.textMuted)
                }
                
                Spacer()
                
                if isFocused {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(isDestructive ? Color.red : NXTheme.secondary)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(
                        isFocused
                            ? (isDestructive ? Color.red.opacity(0.18) : NXTheme.glassSurfaceElevated.opacity(0.95))
                            : Color.clear
                    )
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(
                        isFocused
                            ? (isDestructive ? Color.red.opacity(0.5) : NXTheme.secondary.opacity(0.7))
                            : Color.clear,
                        lineWidth: 1.2
                    )
            )
        }
        .buttonStyle(.plain)
    }
    
    func controllerHint(button: String, action: String) -> some View {
        HStack(spacing: 4) {
            Text(button)
                .font(.system(size: 9, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 5)
                .padding(.vertical, 2)
                .background(Circle().fill(Color.white.opacity(0.15)))
            
            Text(action)
                .font(.system(size: 10, weight: .medium, design: .rounded))
                .foregroundColor(NXTheme.textMuted)
        }
    }
}
