import SwiftUI

public struct GlassCard<Content: View>: View {
    private let content: Content
    private let cornerRadius: CGFloat
    private let padding: CGFloat
    
    public init(
        cornerRadius: CGFloat = 14,
        padding: CGFloat = 16,
        @ViewBuilder content: () -> Content
    ) {
        self.cornerRadius = cornerRadius
        self.padding = padding
        self.content = content()
    }
    
    public var body: some View {
        content
            .padding(padding)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                        .fill(NXTheme.glassSurface.opacity(0.85))
                    RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 1)
            )
            .shadow(color: Color.black.opacity(0.35), radius: 10, x: 0, y: 4)
    }
}

public struct GlassGroupBoxStyle: GroupBoxStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            configuration.label
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            configuration.content
        }
        .padding(14)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(NXTheme.glassSurface.opacity(0.85))
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 1)
        )
        .shadow(color: Color.black.opacity(0.3), radius: 8, x: 0, y: 3)
    }
}
