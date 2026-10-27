import SwiftUI
import AppKit

public struct CompatibleGameGridCard: View {
    public let item: CompatibleGameItem
    public let isCardFocused: Bool
    public let focusedButton: CompatibilityButtonFocus?
    public let onNotes: () -> Void
    
    public init(
        item: CompatibleGameItem,
        isCardFocused: Bool = false,
        focusedButton: CompatibilityButtonFocus? = nil,
        onNotes: @escaping () -> Void = {}
    ) {
        self.item = item
        self.isCardFocused = isCardFocused
        self.focusedButton = focusedButton
        self.onNotes = onNotes
    }
    
    public var body: some View {
        GlassCard(padding: 10) {
            VStack(alignment: .leading, spacing: 8) {
                // MARK: - Game Cover Art in Portrait (1:1.35 ratio)
                coverArtView
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .stroke(NXTheme.specularRim, lineWidth: 0.8)
                    )
                
                // MARK: - Title (Fixed height for uniform grid)
                Text(item.title)
                    .font(.system(size: 12, weight: .bold, design: .rounded))
                    .foregroundColor(isCardFocused ? .white : NXTheme.textPrimary)
                    .lineLimit(2)
                    .frame(height: 32, alignment: .topLeading)
                
                // MARK: - Hardware Requirements (Mac & iOS Specs)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 4) {
                        Text(item.macReq)
                            .font(.system(size: 8, weight: .semibold, design: .rounded))
                            .padding(.horizontal, 5)
                            .padding(.vertical, 2)
                            .background(Color.blue.opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                        
                        Text(item.storageReq)
                            .font(.system(size: 8, weight: .semibold, design: .monospaced))
                            .padding(.horizontal, 5)
                            .padding(.vertical, 2)
                            .background(Color.purple.opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                    }
                    
                    Text(item.iosReq)
                        .font(.system(size: 8, weight: .medium, design: .rounded))
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1.5)
                        .background(Color.orange.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                }
                .frame(height: 34, alignment: .leading)
                
                Divider().opacity(0.2)
                
                // MARK: - Footer: Action Button (Notes only, Port Game removed)
                HStack {
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
                        .controlSize(.mini)
                        .overlay(
                            RoundedRectangle(cornerRadius: 6)
                                .stroke(isCardFocused && focusedButton == .notes ? Color.white : Color.clear, lineWidth: 1.5)
                        )
                    }
                    
                    Spacer()
                    
                    Text("v\(item.versionTested)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(NXTheme.textMuted)
                }
            }
        }
        .frame(maxWidth: .infinity)
        .frame(height: 318)
        .clipped()
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(isCardFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.clear), lineWidth: 2)
        )
        .shadow(color: isCardFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 3)
    }
    
    // MARK: - Cover Art Loader (Strictly authentic portrait cover)
    @ViewBuilder
    private var coverArtView: some View {
        GeometryReader { geo in
            if let localImg = GameCoverService.shared.coverImage(titleId: item.titleId, gameName: item.title) {
                Image(nsImage: localImg)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: geo.size.width, height: geo.size.height)
                    .clipped()
            } else {
                fallbackIcon
                    .frame(width: geo.size.width, height: geo.size.height)
            }
        }
        .frame(height: 185)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
    
    private var fallbackIcon: some View {
        ZStack {
            NXTheme.primary.opacity(0.2)
            VStack(spacing: 6) {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 26))
                    .foregroundColor(NXTheme.secondary)
                Text(item.title)
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.white)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 8)
                    .lineLimit(2)
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
