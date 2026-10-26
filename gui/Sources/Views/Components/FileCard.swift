import SwiftUI

public struct FileCard: View {
    public let item: GameFileItem
    
    public init(item: GameFileItem) {
        self.item = item
    }
    
    public var body: some View {
        HStack(spacing: 12) {
            Image(systemName: item.type.iconName)
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 20))
                .foregroundColor(colorForType(item.type))
                .frame(width: 26)
            
            VStack(alignment: .leading, spacing: 3) {
                Text(item.name)
                    .font(.system(size: 12, weight: .medium, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                
                HStack(spacing: 6) {
                    Text(item.type.rawValue)
                        .font(.system(size: 10, weight: .bold))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(colorForType(item.type).opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(Capsule())
                    
                    if let ver = item.versionString {
                        Text(ver)
                            .font(.system(size: 10, weight: .semibold, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    if let tid = item.titleId {
                        Text("[\(tid)]")
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(NXTheme.textMuted)
                    }
                }
            }
            
            Spacer()
            
            Text(item.formattedSize)
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundColor(NXTheme.textSecondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(NXTheme.glassSurfaceElevated.opacity(0.75))
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(.ultraThinMaterial)
            }
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private func colorForType(_ type: GameFileType) -> Color {
        switch type {
        case .base: return NXTheme.primary
        case .update: return NXTheme.secondary
        case .dlc: return NXTheme.tertiary
        case .unknown: return NXTheme.textMuted
        }
    }
}
