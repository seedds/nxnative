import SwiftUI

public struct SelectableGameCard: View {
    public let game: SelectableGame
    public let isSelected: Bool
    public let onSelect: () -> Void
    
    public init(game: SelectableGame, isSelected: Bool, onSelect: @escaping () -> Void) {
        self.game = game
        self.isSelected = isSelected
        self.onSelect = onSelect
    }
    
    public var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 12) {
                // Checkbox / selection indicator
                Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundColor(isSelected ? NXTheme.secondary : NXTheme.textMuted)
                    .frame(width: 24)
                
                // Official Portrait Cover Thumbnail
                if let cover = GameCoverService.shared.coverImage(titleId: game.titleId, gameName: game.name) {
                    Image(nsImage: cover)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 30, height: 46)
                        .clipped()
                        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 6, style: .continuous)
                                .stroke(NXTheme.specularRim, lineWidth: 0.7)
                        )
                }
                
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 8) {
                        Text(game.name)
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                            .lineLimit(1)
                        
                        if let tid = game.titleId {
                            Text("[\(tid)]")
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(NXTheme.textMuted)
                        }
                    }
                    
                    HStack(spacing: 6) {
                        // Badge: Base (White text for high contrast per user request)
                        if game.baseFile != nil {
                            Text("Base")
                                .font(.system(size: 10, weight: .bold))
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(NXTheme.primary.opacity(0.35))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                        
                        // Badge: Updates (White text for high contrast)
                        if game.updateCount > 0 {
                            Text("+\(game.updateCount) Update\(game.updateCount > 1 ? "s" : "")")
                                .font(.system(size: 10, weight: .bold))
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(NXTheme.secondary.opacity(0.35))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                        
                        // Badge: DLCs (White text for high contrast)
                        if game.dlcCount > 0 {
                            Text("+\(game.dlcCount) DLC\(game.dlcCount > 1 ? "s" : "")")
                                .font(.system(size: 10, weight: .bold))
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(NXTheme.tertiary.opacity(0.4))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                        
                        Text("(\(game.files.count) file\(game.files.count > 1 ? "s" : ""))")
                            .font(.system(size: 10))
                            .foregroundColor(NXTheme.textMuted)
                    }
                }
                
                Spacer()
                
                Text(game.formattedSize)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundColor(isSelected ? NXTheme.textPrimary : NXTheme.textSecondary)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(isSelected ? NXTheme.primary.opacity(0.18) : NXTheme.glassSurfaceElevated.opacity(0.7))
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(
                        isSelected ? NXTheme.selectedBorder : NXTheme.specularRim,
                        lineWidth: isSelected ? 1.4 : 0.8
                    )
            )
            .shadow(color: isSelected ? NXTheme.primary.opacity(0.3) : Color.clear, radius: 8, x: 0, y: 2)
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
