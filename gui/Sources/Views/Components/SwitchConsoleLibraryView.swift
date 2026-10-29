import SwiftUI
import AppKit

public struct SwitchConsoleLibraryView: View {
    @ObservedObject public var viewModel: AppViewModel
    public let games: [PortedGame]
    @State private var scrollWheelMonitor: Any? = nil
    @State private var suppressVerticalAutoScroll: Bool = false

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
        ScrollViewReader { verticalProxy in
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 18) {
                    // MARK: - 1. Hero Section (Widescreen 16:9 Promo Art + Game Controls)
                    if let selected = focusedGame {
                        ConsoleHeroView(game: selected, viewModel: viewModel, height: 350)
                            .id("hero_top")
                            .frame(height: 350)
                            .transition(.opacity.animation(.easeInOut(duration: 0.22)))
                    }

                    // MARK: - 2. Two Categories Shelves (Row 1: Mac, Row 2: iPhone / iPad)
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
                    .padding(.bottom, 32)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .nxScrollEdgeEffect(edges: [.top, .bottom])
            .onChange(of: viewModel.focusedGameIndex) { _ in
                if suppressVerticalAutoScroll {
                    suppressVerticalAutoScroll = false
                    return
                }
                if let focused = focusedGame {
                    withAnimation(.easeInOut(duration: 0.22)) {
                        if focused.platform == .macApp {
                            verticalProxy.scrollTo("shelf_mac", anchor: .center)
                        } else {
                            verticalProxy.scrollTo("shelf_ios", anchor: .bottom)
                        }
                    }
                }
            }
            .onAppear {
                installScrollWheelForwarder()
            }
            .onDisappear {
                removeScrollWheelForwarder()
            }
        }
    }

    /// Forwards vertical mouse-wheel scrolling over nested horizontal carousels to the outer vertical NSScrollView.
    private func installScrollWheelForwarder() {
        guard scrollWheelMonitor == nil else { return }
        scrollWheelMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
            guard !viewModel.isAnyModalPresented, viewModel.selectedTab == 1 else {
                return event
            }
            // Allow Shift + Wheel or horizontal trackpad swipes to scroll the horizontal shelf normally.
            guard !event.modifierFlags.contains(.shift),
                  abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX),
                  abs(event.scrollingDeltaY) > 0.01,
                  let window = event.window,
                  let hitView = window.contentView?.hitTest(event.locationInWindow) else {
                return event
            }

            // Walk ancestor chain: if hit inside a horizontal NSScrollView nested inside an outer vertical NSScrollView,
            // forward the vertical wheel event directly to the outer vertical NSScrollView.
            var scrollViews: [NSScrollView] = []
            var cursor: NSView? = hitView
            while let current = cursor {
                if let sv = current as? NSScrollView {
                    scrollViews.append(sv)
                }
                cursor = current.superview
            }

            if scrollViews.count >= 2 {
                let outerVerticalScrollView = scrollViews[1]
                outerVerticalScrollView.scrollWheel(with: event)
                return nil
            }
            return event
        }
    }

    private func removeScrollWheelForwarder() {
        if let monitor = scrollWheelMonitor {
            NSEvent.removeMonitor(monitor)
            scrollWheelMonitor = nil
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
        let baseCardSize: CGFloat = 144 * CGFloat(viewModel.libraryCardScale)
        let verticalPadding: CGFloat = max(22, baseCardSize * 0.14)
        let cardSpacing: CGFloat = max(20, baseCardSize * 0.13)

        VStack(alignment: .leading, spacing: 6) {
            // Category Header
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(categoryColor)

                Text(title)
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)

                Text("· \(count) \(countSuffix)")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.white.opacity(0.85))

                Spacer()
            }
            .padding(.horizontal, 34)

            // Horizontal Scroll Carousel of Square 1:1 Nintendo Switch Tiles
            ScrollViewReader { horizontalProxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(alignment: .center, spacing: cardSpacing) {
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
                                        suppressVerticalAutoScroll = true
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
                    .padding(.horizontal, 36)
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
