import SwiftUI
import AppKit

public struct ConsoleGameCarouselView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var coverService = GameCoverService.shared
    public let games: [PortedGame]
    
    public init(viewModel: AppViewModel, games: [PortedGame]) {
        self.viewModel = viewModel
        self.games = games
    }
    
    public var body: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 32) {
                    ForEach(Array(games.enumerated()), id: \.element.id) { index, game in
                        let isFocused = viewModel.focusedGameIndex == index
                        let isLaunching = viewModel.launchingGameId == game.id
                        
                        gameCoverCard(game: game, index: index, isFocused: isFocused, isLaunching: isLaunching)
                            .id(index)
                            .onTapGesture {
                                SoundEffectService.shared.play(.focus)
                                withAnimation(.spring(response: 0.28, dampingFraction: 0.75)) {
                                    if viewModel.focusedGameIndex == index {
                                        viewModel.launchGame(game)
                                    } else {
                                        viewModel.focusedGameIndex = index
                                        viewModel.isFocusOnDeck = false
                                    }
                                }
                            }
                    }
                }
                .padding(.horizontal, 40)
                .padding(.top, 28)
                .padding(.bottom, 20)
            }
            .onChange(of: viewModel.focusedGameIndex) { newIndex in
                withAnimation(.spring(response: 0.28, dampingFraction: 0.8)) {
                    proxy.scrollTo(newIndex, anchor: .center)
                }
            }
        }
        .frame(height: LibraryCardSize.carouselHeight(scale: viewModel.libraryCardScale))
    }
    
    private func gameCoverCard(game: PortedGame, index: Int, isFocused: Bool, isLaunching: Bool) -> some View {
        let cardImage = coverService.coverImage(for: game) ?? NSWorkspace.shared.icon(forFile: game.url.path)
        let cardWidth = isFocused
            ? LibraryCardSize.focusedWidth(scale: viewModel.libraryCardScale)
            : LibraryCardSize.unfocusedWidth(scale: viewModel.libraryCardScale)
        let cardHeight = isFocused
            ? LibraryCardSize.focusedHeight(scale: viewModel.libraryCardScale)
            : LibraryCardSize.unfocusedHeight(scale: viewModel.libraryCardScale)
        let cornerRad: CGFloat = isFocused ? 20 : 16
        let isSquareIcon = abs(cardImage.size.width - cardImage.size.height) < 10
        
        return VStack(spacing: 12) {
            ZStack(alignment: .top) {
                // Background Ambient Glow for Focused Card
                if isFocused {
                    RoundedRectangle(cornerRadius: cornerRad + 4, style: .continuous)
                        .fill(NXTheme.primaryGradient.opacity(0.45))
                        .frame(width: cardWidth + 18, height: cardHeight + 18)
                        .blur(radius: 16)
                }
                
                // Game Cover Portrait Artwork (Official Switch Box Art - 1 : 1.60 ratio)
                Group {
                    if isSquareIcon {
                        // Styled portrait box container for fallback icons so they never appear cropped
                        ZStack {
                            RoundedRectangle(cornerRadius: cornerRad, style: .continuous)
                                .fill(
                                    LinearGradient(
                                        colors: [Color(white: 0.18), Color(white: 0.08)],
                                        startPoint: .top,
                                        endPoint: .bottom
                                    )
                                )
                            
                            Image(nsImage: cardImage)
                                .resizable()
                                .aspectRatio(contentMode: .fit)
                                .frame(width: cardWidth * 0.72, height: cardWidth * 0.72)
                                .shadow(color: Color.black.opacity(0.55), radius: 10, x: 0, y: 5)
                        }
                    } else {
                        // Full-bleed official portrait cover art
                        Image(nsImage: cardImage)
                            .resizable()
                            .aspectRatio(contentMode: .fill)
                    }
                }
                .frame(width: cardWidth, height: cardHeight)
                .clipped()
                .clipShape(RoundedRectangle(cornerRadius: cornerRad, style: .continuous))
                .shadow(
                    color: isFocused ? NXTheme.primary.opacity(0.8) : Color.black.opacity(0.45),
                    radius: isFocused ? 24 : 8,
                    x: 0,
                    y: isFocused ? 12 : 4
                )
                .overlay(
                    RoundedRectangle(cornerRadius: cornerRad, style: .continuous)
                        .strokeBorder(
                            isFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.white.opacity(0.15)),
                            lineWidth: isFocused ? 3.5 : 1
                        )
                )
                
                // Active Launching / Playing Pill
                if isLaunching {
                    HStack(spacing: 5) {
                        ProgressView()
                            .controlSize(.mini)
                        Text("Launching")
                            .font(.system(size: 10, weight: .bold, design: .rounded))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.85))
                    .clipShape(Capsule())
                    .overlay(
                        Capsule()
                            .strokeBorder(NXTheme.secondary, lineWidth: 1)
                    )
                    .padding(.top, 10)
                }
                
                // Platform Badge at Bottom of Cover
                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        Text(game.platform == .macApp ? "Mac App" : "iOS IPA")
                            .font(.system(size: 9, weight: .bold, design: .rounded))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(Color.black.opacity(0.75))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                            .overlay(
                                Capsule().stroke(Color.white.opacity(0.2), lineWidth: 0.5)
                            )
                    }
                    .padding(10)
                }
                .frame(width: cardWidth, height: cardHeight)
            }
            .scaleEffect(isFocused ? 1.04 : 1.0)
            .animation(.spring(response: 0.28, dampingFraction: 0.72), value: isFocused)
            
            // Game Name (Centered, high contrast, prominent)
            VStack(spacing: 3) {
                Text(game.name)
                    .font(.system(size: isFocused ? 16 : 13, weight: isFocused ? .bold : .medium, design: .rounded))
                    .foregroundColor(isFocused ? NXTheme.textPrimary : NXTheme.textSecondary)
                    .lineLimit(1)
                    .multilineTextAlignment(.center)
                
                Text(game.formattedSize)
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(NXTheme.textMuted)
            }
            .frame(width: cardWidth + 20)
        }
        .contentShape(Rectangle())
    }
}
