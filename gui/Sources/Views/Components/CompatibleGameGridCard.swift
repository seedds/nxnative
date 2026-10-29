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
        GlassCard(padding: 8, tint: isCardFocused ? NXTheme.primary.opacity(0.28) : nil) {
            VStack(alignment: .leading, spacing: 6) {
                // MARK: - Game Cover Art in Portrait (1:1.35 ratio)
                coverArtView
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .stroke(NXTheme.specularRim, lineWidth: 0.8)
                    )
                    .overlay(alignment: .topTrailing) {
                        if item.isNew {
                            Text(L10n.t("compatibility.new_badge"))
                                .font(.system(size: 8, weight: .heavy, design: .rounded))
                                .foregroundColor(.white)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 2)
                                .background(
                                    Capsule()
                                        .fill(NXTheme.primaryGradient)
                                )
                                .shadow(color: Color.black.opacity(0.45), radius: 3, x: 0, y: 1)
                                .padding(5)
                        }
                    }
                
                // MARK: - Title (Fixed height for uniform grid)
                Text(item.title)
                    .font(.system(size: 11, weight: .bold, design: .rounded))
                    .foregroundColor(isCardFocused ? .white : NXTheme.textPrimary)
                    .lineLimit(2)
                    .frame(height: 28, alignment: .topLeading)
                
                // MARK: - Hardware Requirements (Mac & iOS Specs)
                VStack(alignment: .leading, spacing: 2.5) {
                    HStack(spacing: 3) {
                        Text(item.macReq)
                            .font(.system(size: 7.5, weight: .semibold, design: .rounded))
                            .padding(.horizontal, 4.5)
                            .padding(.vertical, 1.5)
                            .background(Color.blue.opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 3.5))
                        
                        Text(item.storageReq)
                            .font(.system(size: 7.5, weight: .semibold, design: .monospaced))
                            .padding(.horizontal, 4.5)
                            .padding(.vertical, 1.5)
                            .background(Color.purple.opacity(0.35))
                            .foregroundColor(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 3.5))
                    }
                    
                    Text(item.iosReq)
                        .font(.system(size: 7.5, weight: .medium, design: .rounded))
                        .padding(.horizontal, 4.5)
                        .padding(.vertical, 1.5)
                        .background(Color.orange.opacity(0.35))
                        .foregroundColor(.white)
                        .clipShape(RoundedRectangle(cornerRadius: 3.5))
                }
                .frame(height: 32, alignment: .leading)
                
                Divider().opacity(0.2)
                
                // MARK: - Footer: Action Button (Notes only, Port Game removed)
                HStack {
                    if item.githubUrl != nil {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            onNotes()
                        }) {
                            HStack(spacing: 3) {
                                Text(L10n.t("compatibility.notes"))
                                Image(systemName: "arrow.up.right")
                            }
                            .font(.system(size: 9.5, weight: .semibold))
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
                        .font(.system(size: 8.5, design: .monospaced))
                        .foregroundColor(NXTheme.textMuted)
                }
            }
        }
        .frame(maxWidth: .infinity)
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(isCardFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.clear), lineWidth: 2)
        )
        .shadow(color: isCardFocused ? NXTheme.primary.opacity(0.4) : Color.clear, radius: 8, x: 0, y: 3)
    }
    
    // MARK: - Game Cover Art (1:1 Square matching Play Screen)
    @ViewBuilder
    private var coverArtView: some View {
        let coverImg = ArtworkStore.shared.loadSquareIcon(titleId: item.titleId, gameName: item.title)
            ?? GameCoverService.shared.coverImage(titleId: item.titleId, gameName: item.title)
        
        Color.clear
            .aspectRatio(1.0, contentMode: .fit)
            .overlay(
                GeometryReader { geo in
                    let side = min(geo.size.width, geo.size.height)
                    Group {
                        if let img = coverImg {
                            Image(nsImage: img)
                                .resizable()
                                .scaledToFill()
                                .frame(width: side, height: side, alignment: .top)
                                .clipped()
                        } else {
                            fallbackIcon
                                .frame(width: side, height: side)
                        }
                    }
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                }
            )
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(NXTheme.specularRim, lineWidth: 0.8)
            )
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
