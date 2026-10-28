import SwiftUI
import AppKit

public struct SquareGameCardView: View {
    public let game: PortedGame
    public let isFocused: Bool
    public let baseSize: CGFloat
    public let onSelect: () -> Void
    @ObservedObject private var artworkStore = ArtworkStore.shared

    public init(
        game: PortedGame,
        isFocused: Bool,
        baseSize: CGFloat = 170,
        onSelect: @escaping () -> Void
    ) {
        self.game = game
        self.isFocused = isFocused
        self.baseSize = baseSize
        self.onSelect = onSelect
    }

    private var iconImage: NSImage {
        ArtworkStore.shared.loadSquareIcon(for: game)
    }

    public var body: some View {
        Button(action: onSelect) {
            ZStack(alignment: .bottomTrailing) {
                // MARK: - 1:1 Square Card Canvas
                if iconImage.size.height > iconImage.size.width * 1.15 {
                    // Portrait box art inside 1:1 square: unclipped fit with blurred glass ambient backdrop
                    ZStack {
                        Color.black.opacity(0.85)

                        Image(nsImage: iconImage)
                            .resizable()
                            .scaledToFill()
                            .frame(width: baseSize, height: baseSize)
                            .blur(radius: 14)
                            .opacity(0.45)
                            .clipped()

                        Image(nsImage: iconImage)
                            .resizable()
                            .scaledToFit()
                            .frame(width: baseSize * 0.90, height: baseSize * 0.90)
                            .shadow(color: Color.black.opacity(0.65), radius: 6, x: 0, y: 3)
                    }
                    .frame(width: baseSize, height: baseSize)
                    .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                } else {
                    // True 1:1 Square Game Icon
                    Image(nsImage: iconImage)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: baseSize, height: baseSize)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                }

                // Ambient gradient on bottom edge for subtle depth
                LinearGradient(
                    colors: [Color.clear, Color.black.opacity(0.4)],
                    startPoint: .center,
                    endPoint: .bottom
                )
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))

                // Platform pill badge (Mac Native vs iOS) & Cloud Save Status
                HStack(spacing: 4) {
                    Text(game.platform == .macApp ? "MAC" : "IOS")
                        .font(.system(size: 8, weight: .black, design: .rounded))
                        .foregroundColor(.white)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 2)
                        .background(game.platform == .macApp ? NXTheme.primary.opacity(0.85) : NXTheme.secondary.opacity(0.85))
                        .clipShape(Capsule())

                    if SaveSyncService.shared.isCloudSyncActive(for: game) {
                        Image(systemName: "icloud.fill")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 2)
                            .background(Color.blue.opacity(0.85))
                            .clipShape(Capsule())
                    }
                }
                .padding(8)
            }
            .frame(width: baseSize, height: baseSize)
            // MARK: - Switch Dark Mode Glowing Focus Frame
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(
                        isFocused ? NXTheme.primary : Color.white.opacity(0.12),
                        lineWidth: isFocused ? 4 : 1
                    )
            )
            .shadow(
                color: isFocused ? NXTheme.primary.opacity(0.9) : Color.black.opacity(0.3),
                radius: isFocused ? 16 : 4,
                x: 0,
                y: isFocused ? 4 : 2
            )
            .scaleEffect(isFocused ? 1.10 : 1.0)
            .animation(.spring(response: 0.28, dampingFraction: 0.76), value: isFocused)
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
