import SwiftUI

public struct PathPickerRow: View {
    public let title: String
    public let icon: String
    public let path: String?
    public let placeholder: String
    public let actionTitle: String
    public let onSelect: () -> Void
    
    public init(
        title: String,
        icon: String,
        path: String?,
        placeholder: String,
        actionTitle: String = "Choose...",
        onSelect: @escaping () -> Void
    ) {
        self.title = title
        self.icon = icon
        self.path = path
        self.placeholder = placeholder
        self.actionTitle = actionTitle
        self.onSelect = onSelect
    }
    
    public var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(title, systemImage: icon)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(NXTheme.textSecondary)
            
            HStack(spacing: 8) {
                HStack {
                    if let p = path, !p.isEmpty {
                        Text(p)
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(NXTheme.textPrimary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    } else {
                        Text(placeholder)
                            .font(.system(size: 12))
                            .foregroundColor(NXTheme.textMuted)
                    }
                    Spacer()
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(NXTheme.glassSurfaceElevated)
                .cornerRadius(8)
                .overlay(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                )
                
                Button(action: onSelect) {
                    Text(actionTitle)
                }
                .buttonStyle(NXSecondaryButtonStyle())
            }
        }
    }
}
