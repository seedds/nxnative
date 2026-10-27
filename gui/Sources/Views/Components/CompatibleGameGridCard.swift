import SwiftUI
import AppKit

public struct CompatibleGameGridCard: View {
    public let item: CompatibleGameItem
    public let isCardFocused: Bool
    public let focusedButton: CompatibilityButtonFocus?
    public let onNotes: () -> Void
    public let onPort: () -> Void
    
    public init(
        item: CompatibleGameItem,
        isCardFocused: Bool = false,
        focusedButton: CompatibilityButtonFocus? = nil,
        onNotes: @escaping () -> Void = {},
        onPort: @escaping () -> Void = {}
    ) {
        self.item = item
        self.isCardFocused = isCardFocused
        self.focusedButton = focusedButton
        self.onNotes = onNotes
        self.onPort = onPort
    }
    
    public var body: some View {
        GlassCard(padding: 12) {
            VStack(alignment: .leading, spacing: 10) {
                // MARK: - Game Cover Art in Portrait (~1 : 1.35 ratio)
                ZStack(alignment: .topTrailing) {
                    coverArtView
                        .frame(maxWidth: .infinity)
                        .frame(height: 240)
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
                        .foregroundColor(isCardFocused ? .white : NXTheme.textPrimary)
                        .lineLimit(2)
                        .frame(minHeight: 34, alignment: .topLeading)
                    
                    Text(item.titleId)
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundColor(NXTheme.textSecondary)
                }
                
                Divider().opacity(0.2)
                
                // MARK: - Footer: Action Buttons (Notes & Port Game)
                HStack(spacing: 8) {
                    if item.githubUrl != nil {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            onNotes()
                        }) {
                            HStack(spacing: 3) {
                                Text("Notes")
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 10, weight: .semibold))
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)
                        .overlay(
                            RoundedRectangle(cornerRadius: 8)
                                .stroke(isCardFocused && focusedButton == .notes ? Color.white : Color.clear, lineWidth: 1.5)
                        )
                    }
                    
                    Spacer()
                    
                    Button("Port Game") {
                        SoundEffectService.shared.play(.click)
                        onPort()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .controlSize(.small)
                    .overlay(
                        RoundedRectangle(cornerRadius: 10)
                            .stroke(isCardFocused && focusedButton == .portGame ? Color.white : Color.clear, lineWidth: 2)
                    )
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .top)
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(isCardFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.clear), lineWidth: 2)
        )
        .shadow(color: isCardFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 3)
    }
    
    // MARK: - Cover Art Loader
    @ViewBuilder
    private var coverArtView: some View {
        if let localImg = GameCoverService.shared.coverImage(titleId: item.titleId, gameName: item.title) {
            let isPortrait = localImg.size.height >= localImg.size.width * 1.2
            if isPortrait {
                Image(nsImage: localImg)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
                    .clipped()
            } else {
                ZStack {
                    Image(nsImage: localImg)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .blur(radius: 20)
                        .opacity(0.35)
                    Image(nsImage: localImg)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .padding(8)
                }
                .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
                .clipped()
            }
        } else if let url = item.coverArtUrl {
            AsyncImage(url: url) { phase in
                switch phase {
                case .success(let image):
                    image
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
                        .clipped()
                case .empty:
                    ZStack {
                        NXTheme.glassSurfaceElevated
                        ProgressView().controlSize(.small)
                    }
                    .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
                default:
                    fallbackIcon
                        .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
                }
            }
        } else {
            fallbackIcon
                .frame(maxWidth: .infinity, minHeight: 240, maxHeight: 240)
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
                    .foregroundColor(.white)
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
