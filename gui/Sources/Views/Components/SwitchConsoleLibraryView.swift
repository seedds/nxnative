import SwiftUI
import AppKit

public struct SwitchConsoleLibraryView: View {
    @ObservedObject public var viewModel: AppViewModel
    public let games: [PortedGame]
    @State private var selectedGameId: UUID? = nil

    public init(viewModel: AppViewModel, games: [PortedGame]) {
        self.viewModel = viewModel
        self.games = games
    }

    private var macGames: [PortedGame] {
        games.filter { $0.platform == .macApp }
    }

    private var iosGames: [PortedGame] {
        games.filter { $0.platform == .ios }
    }

    private var focusedGame: PortedGame? {
        guard !games.isEmpty else { return nil }
        if let selId = selectedGameId, let match = games.first(where: { $0.id == selId }) {
            return match
        }
        let idx = max(0, min(viewModel.focusedGameIndex, games.count - 1))
        return games[idx]
    }

    public var body: some View {
        GeometryReader { geo in
            let totalHeight = geo.size.height
            let heroHeight = max(240, totalHeight * 0.48)
            let shelfHeight = totalHeight - heroHeight + 40

            ZStack(alignment: .bottom) {
                // MARK: - 1. Hero Section (Widescreen 16:9 Promo Art + Game Controls)
                VStack(spacing: 0) {
                    if let selected = focusedGame {
                        ConsoleHeroView(game: selected, viewModel: viewModel, height: heroHeight)
                            .id("hero_\(selected.id.uuidString)")
                            .transition(.opacity.animation(.easeInOut(duration: 0.22)))
                    } else {
                        Spacer().frame(height: heroHeight)
                    }
                    Spacer()
                }
                .frame(width: geo.size.width, height: totalHeight)

                // MARK: - 2. Smooth Blend Gradient Between Hero & Shelves
                LinearGradient(
                    stops: [
                        .init(color: Color.clear, location: 0.0),
                        .init(color: Color.nxCanvas.opacity(0.65), location: 0.35),
                        .init(color: Color.nxCanvas, location: 1.0)
                    ],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .frame(height: shelfHeight + 50)
                .allowsHitTesting(false)

                // MARK: - 3. Two Categories Shelves (Row 1: Mac, Row 2: iPhone / iPad)
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 22) {
                        // Category 1: Mac (macOS Native)
                        if !macGames.isEmpty {
                            categoryShelf(
                                title: "Mac (macOS Native)",
                                icon: "laptopcomputer",
                                count: macGames.count,
                                countSuffix: macGames.count == 1 ? "game" : "games",
                                categoryColor: NXTheme.primary,
                                items: macGames
                            )
                        }

                        // Category 2: iPhone / iPad (iOS Port)
                        if !iosGames.isEmpty {
                            categoryShelf(
                                title: "iPhone / iPad (iOS Port)",
                                icon: "iphone.gen3",
                                count: iosGames.count,
                                countSuffix: iosGames.count == 1 ? "port" : "ports",
                                categoryColor: NXTheme.secondary,
                                items: iosGames
                            )
                        }

                        // Fallback if both empty
                        if macGames.isEmpty && iosGames.isEmpty {
                            VStack(spacing: 12) {
                                Image(systemName: "gamecontroller")
                                    .font(.system(size: 38))
                                    .foregroundColor(NXTheme.textMuted)
                                Text("Nenhum jogo encontrado na biblioteca")
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                            .frame(maxWidth: .infinity, minHeight: 140)
                        }
                    }
                    .padding(.top, 24)
                    .padding(.bottom, 28)
                }
                .frame(height: shelfHeight)
            }
            .frame(width: geo.size.width, height: totalHeight)
            .background(Color.nxCanvas)
        }
    }

    @ViewBuilder
    private func categoryShelf(
        title: String,
        icon: String,
        count: Int,
        countSuffix: String,
        categoryColor: Color,
        items: [PortedGame]
    ) -> some View {
        let baseCardSize: CGFloat = 138 * CGFloat(viewModel.libraryCardScale)

        VStack(alignment: .leading, spacing: 8) {
            // Category Header
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(categoryColor)

                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)

                Text("· \(count) \(countSuffix)")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(NXTheme.textSecondary)

                Spacer()
            }
            .padding(.horizontal, 34)

            // Horizontal Scroll Carousel of Square 1:1 Cards
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(alignment: .center, spacing: 18) {
                    ForEach(items) { game in
                        let isFocused = focusedGame?.id == game.id

                        SquareGameCardView(
                            game: game,
                            isFocused: isFocused,
                            baseSize: baseCardSize,
                            onSelect: {
                                SoundEffectService.shared.play(.focus)
                                withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
                                    selectedGameId = game.id
                                    if let idx = games.firstIndex(where: { $0.id == game.id }) {
                                        viewModel.focusedGameIndex = idx
                                    }
                                }
                            }
                        )
                        .id("card_\(game.id.uuidString)")
                    }
                }
                .padding(.horizontal, 34)
                .padding(.vertical, 8)
            }
        }
    }
}
