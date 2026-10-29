import SwiftUI

public struct GlassCard<Content: View>: View {
    private let content: Content
    private let cornerRadius: CGFloat
    private let padding: CGFloat
    private let tint: Color?
    private let isNested: Bool
    
    public init(
        cornerRadius: CGFloat = 14,
        padding: CGFloat = 16,
        tint: Color? = nil,
        isNested: Bool = false,
        @ViewBuilder content: () -> Content
    ) {
        self.cornerRadius = cornerRadius
        self.padding = padding
        self.tint = tint
        self.isNested = isNested
        self.content = content()
    }
    
    public var body: some View {
        content
            .padding(padding)
            .nxGlassCard(cornerRadius: cornerRadius, tint: tint, isNested: isNested)
    }
}

public struct GlassGroupBoxStyle: GroupBoxStyle {
    private let isNested: Bool
    
    public init(isNested: Bool = false) {
        self.isNested = isNested
    }
    
    public func makeBody(configuration: Configuration) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            configuration.label
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            configuration.content
        }
        .padding(14)
        .nxGlassCard(cornerRadius: 14, isNested: isNested)
    }
}
