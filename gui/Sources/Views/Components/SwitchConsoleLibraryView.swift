import SwiftUI
import AppKit

public struct SwitchConsoleLibraryView: View {
    @ObservedObject public var viewModel: AppViewModel
    public let games: [PortedGame]

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
        let idx = max(0, min(viewModel.focusedGameIndex, games.count - 1))
        return games[idx]
    }

    public var body: some View {
        GeometryReader { geo in
            let totalHeight = geo.size.height
            // Hero Section: proportional height giving ample breathing room for 16:9 art + action buttons
            let heroHeight = min(420, max(280, totalHeight * 0.48))

            VStack(spacing: 0) {
                // MARK: - 1. Hero Section (Widescreen 16:9 Promo Art + Game Controls)
                if let selected = focusedGame {
                    ConsoleHeroView(game: selected, viewModel: viewModel, height: heroHeight)
                        .id("hero_\(selected.id.uuidString)")
                        .frame(height: heroHeight)
                        .transition(.opacity.animation(.easeInOut(duration: 0.22)))
                } else {
                    Spacer().frame(height: heroHeight)
                }

                // MARK: - 2. Two Categories Shelves (Row 1: Mac, Row 2: iPhone / iPad)
                ScrollViewReader { verticalProxy in
                    ScrollView(.vertical, showsIndicators: false) {
                        VStack(alignment: .leading, spacing: 18) {
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
                                .id("shelf_mac")
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
                                .id("shelf_ios")
                            }

                            // Fallback if both empty
                            if macGames.isEmpty && iosGames.isEmpty {
                                VStack(spacing: 12) {
                                    Image(systemName: "gamecontroller")
                                        .font(.system(size: 38))
                                        .foregroundColor(NXTheme.textMuted)
                                    Text(viewModel.tr("console.no_games"))
                                        .font(.system(size: 14, weight: .semibold))
                                        .foregroundColor(NXTheme.textSecondary)
                                }
                                .frame(maxWidth: .infinity, minHeight: 140)
                            }
                        }
                        .padding(.top, 14)
                        .padding(.bottom, 28)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .onChange(of: viewModel.focusedGameIndex) { _ in
                        if let focused = focusedGame {
                            withAnimation(.easeInOut(duration: 0.22)) {
                                if focused.platform == .macApp {
                                    verticalProxy.scrollTo("shelf_mac", anchor: .top)
                                } else {
                                    verticalProxy.scrollTo("shelf_ios", anchor: .bottom)
                                }
                            }
                        }
                    }
                }
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
        let verticalPadding: CGFloat = max(28, baseCardSize * 0.12 + 18)
        let cardSpacing: CGFloat = max(22, baseCardSize * 0.14)

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

            // Horizontal Scroll Carousel of Square 1:1 Cards (With Auto-Scroll & Non-Clipping Padding)
            ScrollViewReader { horizontalProxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(alignment: .center, spacing: cardSpacing) {
                        ForEach(items) { game in
                            let isFocused = focusedGame?.id == game.id

                            SquareGameCardView(
                                game: game,
                                viewModel: viewModel,
                                isFocused: isFocused,
                                baseSize: baseCardSize,
                                onSelect: {
                                    guard !viewModel.isAnyModalPresented else { return }
                                    if isFocused {
                                        if game.platform == .macApp {
                                            viewModel.launchGame(game)
                                        } else {
                                            SoundEffectService.shared.play(.click)
                                            viewModel.gameForSettingsSheet = game
                                        }
                                    } else {
                                        SoundEffectService.shared.play(.focus)
                                        withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
                                            if let idx = games.firstIndex(where: { $0.id == game.id }) {
                                                viewModel.focusedGameIndex = idx
                                            }
                                        }
                                    }
                                }
                            )
                            .zIndex(isFocused ? 10 : 1)
                            .id("card_\(game.id.uuidString)")
                        }
                    }
                    .padding(.horizontal, 34)
                    .padding(.vertical, verticalPadding)
                }
                .onChange(of: viewModel.focusedGameIndex) { _ in
                    if let focused = focusedGame, items.contains(where: { $0.id == focused.id }) {
                        withAnimation(.easeInOut(duration: 0.22)) {
                            horizontalProxy.scrollTo("card_\(focused.id.uuidString)", anchor: .center)
                        }
                    }
                }
            }
        }
    }
}
