import SwiftUI

public struct StatusBadge: View {
    public let isPositive: Bool
    public let text: String
    public let showText: Bool
    
    public init(isPositive: Bool, text: String, showText: Bool = false) {
        self.isPositive = isPositive
        self.text = text
        self.showText = showText
    }
    
    private var badgeColor: Color {
        isPositive ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.tertiary
    }
    
    public var body: some View {
        if showText {
            HStack(spacing: 5) {
                Circle()
                    .fill(badgeColor)
                    .frame(width: 6, height: 6)
                    .shadow(color: badgeColor.opacity(0.8), radius: 3)
                
                Text(text)
                    .font(.system(size: 11, weight: .semibold, design: .rounded))
                    .foregroundColor(.white)
            }
            .padding(.horizontal, 9)
            .padding(.vertical, 4)
            .background(
                ZStack {
                    Capsule().fill(badgeColor.opacity(0.12))
                    Capsule().fill(.ultraThinMaterial)
                }
            )
            .overlay(
                Capsule().strokeBorder(badgeColor.opacity(0.35), lineWidth: 0.8)
            )
            .help(text)
        } else {
            // Clean icon-only checkmark with system hover tooltip
            Image(systemName: isPositive ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                .foregroundColor(badgeColor)
                .font(.system(size: 16, weight: .bold))
                .shadow(color: badgeColor.opacity(0.4), radius: 4)
                .help(text)
        }
    }
}
