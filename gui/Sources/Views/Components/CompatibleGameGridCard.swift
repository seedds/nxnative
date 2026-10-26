import SwiftUI
import AppKit

public struct CompatibleGameGridCard: View {
    public let item: CompatibleGameItem
    
    public init(item: CompatibleGameItem) {
        self.item = item
    }
    
    public var body: some View {
        GlassCard(padding: 12) {
            VStack(alignment: .leading, spacing: 10) {
                // MARK: - Game Cover Art with Floating Pills
                ZStack(alignment: .topTrailing) {
                    coverArtView
                        .frame(maxWidth: .infinity)
                        .frame(height: 130)
                        .clipped()
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    
                    // Floating Engine & Graphics API Pills
                    HStack {
                        Text(item.engine)
                            .font(.system(size: 8, weight: .bold, design: .rounded))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 3)
                            .background(Color.black.opacity(0.68))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                            .overlay(
                                Capsule().stroke(Color.white.opacity(0.15), lineWidth: 0.5)
                            )
                        
                        Spacer()
                        
                        Text(item.graphicsApi)
                            .font(.system(size: 8, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(apiColor(item.graphicsApi).opacity(0.85))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                    }
                    .padding(8)
                }
                .overlay(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                )
                .shadow(color: Color.black.opacity(0.3), radius: 6, x: 0, y: 3)
                
                // MARK: - Title & TitleID
                VStack(alignment: .leading, spacing: 3) {
                    Text(item.title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .lineLimit(2)
                        .frame(minHeight: 34, alignment: .topLeading)
                    
                    Text(item.titleId)
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundColor(NXTheme.textSecondary)
                }
                
                Divider().opacity(0.2)
                
                // MARK: - Footer: Tested Version & Notes Link
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Tested Build")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundColor(NXTheme.textMuted)
                        Text(item.versionTested.isEmpty ? "Base" : item.versionTested)
                            .font(.system(size: 10, weight: .semibold, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                    
                    if let url = item.githubUrl {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            NSWorkspace.shared.open(url)
                        }) {
                            HStack(spacing: 4) {
                                Text("Notes")
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 10, weight: .semibold))
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)
                    }
                }
            }
        }
    }
    
    // MARK: - Cover Art Loader
    @ViewBuilder
    private var coverArtView: some View {
        if let url = item.coverArtUrl {
            AsyncImage(url: url) { phase in
                switch phase {
                case .success(let image):
                    image
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                case .empty:
                    ZStack {
                        NXTheme.glassSurfaceElevated
                        ProgressView()
                            .controlSize(.small)
                    }
                default:
                    fallbackIcon
                }
            }
        } else {
            fallbackIcon
        }
    }
    
    private var fallbackIcon: some View {
        ZStack {
            NXTheme.primary.opacity(0.2)
            VStack(spacing: 6) {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 28))
                    .foregroundColor(NXTheme.secondary)
                Text(item.title)
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(NXTheme.textSecondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 8)
                    .lineLimit(1)
            }
        }
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
