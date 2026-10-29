import SwiftUI

// MARK: - Row Subviews and Helpers for CompactConsoleActionSheetView
extension CompactConsoleActionSheetView {
    
    func badgePill(text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .semibold, design: .rounded))
            .padding(.horizontal, 7)
            .padding(.vertical, 2.5)
            .background(
                Capsule()
                    .fill(useLiquidGlass ? Color.white.opacity(0.10) : NXTheme.glassSurfaceElevated.opacity(0.8))
            )
            .overlay(
                Capsule()
                    .strokeBorder(Color.white.opacity(useLiquidGlass ? 0.16 : 0.08), lineWidth: 0.7)
            )
            .foregroundColor(.white)
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
        CompactConsoleActionRowButton(
            index: index,
            title: title,
            subtitle: subtitle,
            icon: icon,
            isPrimary: isPrimary,
            isDestructive: isDestructive,
            isLoading: isLoading,
            isFocused: viewModel.gameActionFocusedIndex == index,
            namespace: actionRowNamespace,
            onHoverFocus: { hovering in
                if hovering && viewModel.gameActionFocusedIndex != index {
                    withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
                        viewModel.gameActionFocusedIndex = index
                    }
                }
            },
            action: {
                SoundEffectService.shared.play(.click)
                viewModel.gameActionFocusedIndex = index
                action()
            }
        )
    }
    
    func controllerHint(button: String, action: String) -> some View {
        HStack(spacing: 4) {
            Text(button)
                .font(.system(size: 9, weight: .bold, design: .rounded))
                .foregroundColor(.white)
                .padding(.horizontal, 5)
                .padding(.vertical, 2)
                .background(Capsule().fill(Color.white.opacity(0.18)))
            
            Text(action)
                .font(.system(size: 10, weight: .semibold, design: .rounded))
                .foregroundColor(.white.opacity(0.88))
        }
    }
}

private struct CompactConsoleActionRowButton: View {
    let index: Int
    let title: String
    let subtitle: String
    let icon: String
    let isPrimary: Bool
    let isDestructive: Bool
    let isLoading: Bool
    let isFocused: Bool
    let namespace: Namespace.ID
    let onHoverFocus: (Bool) -> Void
    let action: () -> Void
    
    @State private var isHovered: Bool = false
    
    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 12, style: .continuous)
        let destructiveTint = Color(red: 0.92, green: 0.24, blue: 0.24)
        let isHighlighted = isFocused || isHovered
        
        Button(action: action) {
            HStack(spacing: 14) {
                // Icon Box
                ZStack {
                    RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .fill(
                            isDestructive
                                ? destructiveTint.opacity(isHighlighted ? 0.38 : 0.18)
                                : (isPrimary ? NXTheme.primary.opacity(isHighlighted ? 0.48 : 0.24) : Color.white.opacity(isHighlighted ? 0.18 : 0.09))
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
                                    ? Color(red: 1.0, green: 0.50, blue: 0.50)
                                    : .white
                            )
                    }
                }
                
                // Labels (Crisp White Title & High-Contrast Subtitle)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(
                            isDestructive
                                ? Color(red: 1.0, green: 0.52, blue: 0.52)
                                : .white
                        )
                    
                    Text(subtitle)
                        .font(.system(size: 10.5, weight: .medium, design: .rounded))
                        .foregroundColor(isHighlighted ? .white.opacity(0.96) : .white.opacity(0.82))
                }
                
                Spacer()
                
                if isHighlighted {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(isDestructive ? Color(red: 1.0, green: 0.55, blue: 0.55) : .white)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
            .contentShape(shape)
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .matchedGeometryEffect(id: index, in: namespace, isSource: true)
        .onHover { hovering in
            isHovered = hovering
            onHoverFocus(hovering)
        }
    }
}
