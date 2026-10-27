import SwiftUI
import AppKit

public struct CompatibleGameRow: View {
    public let game: CompatibleGameItem
    public let isRowFocused: Bool
    public let focusedButton: CompatibilityButtonFocus?
    public let onNotes: () -> Void
    
    public init(
        game: CompatibleGameItem,
        isRowFocused: Bool = false,
        focusedButton: CompatibilityButtonFocus? = nil,
        onNotes: @escaping () -> Void = {}
    ) {
        self.game = game
        self.isRowFocused = isRowFocused
        self.focusedButton = focusedButton
        self.onNotes = onNotes
    }
    
    public var body: some View {
        GlassCard(padding: 10) {
            HStack(spacing: 14) {
                // Official Cover Art / Box Art
                coverArtView
                
                // Game Details
                VStack(alignment: .leading, spacing: 4) {
                    Text(game.title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(isRowFocused ? .white : NXTheme.textPrimary)
                        .lineLimit(1)
                    
                    Text("Ver: \(game.versionTested)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(NXTheme.textSecondary)
                }
                
                Spacer(minLength: 12)
                
                // Hardware Specs Badges (Mac, iOS, Storage)
                HStack(spacing: 6) {
                    Text(game.macReq)
                        .font(.system(size: 9, weight: .semibold, design: .rounded))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(Color.blue.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                    
                    Text(game.iosReq)
                        .font(.system(size: 9, weight: .semibold, design: .rounded))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(Color.orange.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                    
                    Text(game.storageReq)
                        .font(.system(size: 9, weight: .semibold, design: .monospaced))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(Color.purple.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                }
                
                // Action Buttons (Notes only)
                if game.githubUrl != nil {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        onNotes()
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "doc.text.fill")
                            Text("Notes")
                        }
                        .font(.system(size: 11, weight: .semibold))
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.small)
                    .overlay(
                        RoundedRectangle(cornerRadius: 8)
                            .stroke(isRowFocused && focusedButton == .notes ? Color.white : Color.clear, lineWidth: 1.5)
                    )
                    .help("View upstream compatibility notes on GitHub")
                }
            }
            .frame(minHeight: 48)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxWidth: .infinity)
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(isRowFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.clear), lineWidth: 2)
        )
        .shadow(color: isRowFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 2)
    }
    
    // MARK: - Cover Art Loader
    @ViewBuilder
    private var coverArtView: some View {
        if let localImg = GameCoverService.shared.coverImage(titleId: game.titleId, gameName: game.title) {
            Image(nsImage: localImg)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: 44, height: 44)
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                )
        } else {
            fallbackIcon
                .frame(width: 44, height: 44)
        }
    }
    
    private var fallbackIcon: some View {
        ZStack {
            NXTheme.glassSurfaceElevated
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 18))
                .foregroundColor(NXTheme.secondary)
        }
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
    
    private func apiColor(_ api: String) -> Color {
        switch api.uppercased() {
        case "NVN": return Color(red: 0.13, green: 0.77, blue: 0.37)
        case "VULKAN": return NXTheme.tertiary
        case "OPENGL": return NXTheme.primary
        default: return NXTheme.secondary
        }
    }
}
