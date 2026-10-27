import SwiftUI
import AppKit

public struct CompatibleGameRow: View {
    public let game: CompatibleGameItem
    public let isRowFocused: Bool
    public let focusedButton: CompatibilityButtonFocus?
    public let onNotes: () -> Void
    public let onPort: () -> Void
    
    public init(
        game: CompatibleGameItem,
        isRowFocused: Bool = false,
        focusedButton: CompatibilityButtonFocus? = nil,
        onNotes: @escaping () -> Void = {},
        onPort: @escaping () -> Void
    ) {
        self.game = game
        self.isRowFocused = isRowFocused
        self.focusedButton = focusedButton
        self.onNotes = onNotes
        self.onPort = onPort
    }
    
    public var body: some View {
        GlassCard(padding: 10) {
            HStack(spacing: 14) {
                // Official Cover Art / Box Art
                coverArtView
                
                // Game Details
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 8) {
                        Text(game.title)
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(isRowFocused ? .white : NXTheme.textPrimary)
                            .lineLimit(1)
                        
                        Text(game.graphicsApi)
                            .font(.system(size: 9, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(apiColor(game.graphicsApi).opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                    }
                    
                    HStack(spacing: 6) {
                        Text(game.titleId)
                            .font(.system(size: 10, weight: .semibold, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                        
                        Text("•")
                            .foregroundColor(NXTheme.textMuted)
                        
                        Text("Ver: \(game.versionTested)")
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                        
                        if !game.sdk.isEmpty && game.sdk != "-" {
                            Text("•")
                                .foregroundColor(NXTheme.textMuted)
                            Text("SDK: \(game.sdk)")
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        
                        Text("•")
                            .foregroundColor(NXTheme.textMuted)
                        
                        HStack(spacing: 3) {
                            Image(systemName: "cpu")
                                .font(.system(size: 9))
                            Text(game.engine)
                                .font(.system(size: 10))
                        }
                        .foregroundColor(NXTheme.textMuted)
                    }
                }
                
                Spacer(minLength: 12)
                
                // Action Buttons
                HStack(spacing: 8) {
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
                    
                    Button("Port Game") {
                        SoundEffectService.shared.play(.click)
                        onPort()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .controlSize(.small)
                    .overlay(
                        RoundedRectangle(cornerRadius: 10)
                            .stroke(isRowFocused && focusedButton == .portGame ? Color.white : Color.clear, lineWidth: 2)
                    )
                }
                .fixedSize(horizontal: true, vertical: false)
            }
            .frame(height: 46)
        }
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
        } else if let url = game.coverArtUrl {
            AsyncImage(url: url) { phase in
                switch phase {
                case .success(let image):
                    image
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 44, height: 44)
                        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                case .empty:
                    ZStack {
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .fill(NXTheme.glassSurfaceElevated)
                        ProgressView()
                            .controlSize(.mini)
                    }
                    .frame(width: 44, height: 44)
                default:
                    fallbackIcon
                }
            }
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(NXTheme.specularRim, lineWidth: 0.8)
            )
            .shadow(color: Color.black.opacity(0.3), radius: 4, x: 0, y: 2)
        } else {
            fallbackIcon
        }
    }
    
    private var fallbackIcon: some View {
        Image(systemName: "gamecontroller.fill")
            .font(.system(size: 18))
            .foregroundColor(NXTheme.secondary)
            .frame(width: 44, height: 44)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(NXTheme.primary.opacity(0.18))
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(NXTheme.specularRim, lineWidth: 0.8)
            )
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
