import SwiftUI
import AppKit

public struct ConsoleGameCarouselView: View {
    @ObservedObject public var viewModel: AppViewModel
    public let games: [PortedGame]
    
    public init(viewModel: AppViewModel, games: [PortedGame]) {
        self.viewModel = viewModel
        self.games = games
    }
    
    public var body: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 28) {
                    ForEach(Array(games.enumerated()), id: \.element.id) { index, game in
                        let isFocused = viewModel.focusedGameIndex == index
                        let isLaunching = viewModel.launchingGameId == game.id
                        
                        gameCoverCard(game: game, index: index, isFocused: isFocused, isLaunching: isLaunching)
                            .id(index)
                            .onTapGesture {
                                SoundEffectService.shared.play(.focus)
                                withAnimation(.spring(response: 0.28, dampingFraction: 0.75)) {
                                    viewModel.focusedGameIndex = index
                                    viewModel.isFocusOnDeck = false
                                }
                            }
                    }
                }
                .padding(.horizontal, 36)
                .padding(.vertical, 24)
            }
            .onChange(of: viewModel.focusedGameIndex) { newIndex in
                withAnimation(.spring(response: 0.28, dampingFraction: 0.8)) {
                    proxy.scrollTo(newIndex, anchor: .center)
                }
            }
        }
        .frame(height: 310)
    }
    
    private func gameCoverCard(game: PortedGame, index: Int, isFocused: Bool, isLaunching: Bool) -> some View {
        let icon = NSWorkspace.shared.icon(forFile: game.url.path)
        let cardSize: CGFloat = isFocused ? 224 : 168
        let cornerRad: CGFloat = isFocused ? 24 : 18
        
        return VStack(spacing: 12) {
            ZStack(alignment: .top) {
                // Background Ambient Glow for Focused Card
                if isFocused {
                    RoundedRectangle(cornerRadius: cornerRad + 4, style: .continuous)
                        .fill(NXTheme.primaryGradient.opacity(0.4))
                        .frame(width: cardSize + 16, height: cardSize + 16)
                        .blur(radius: 14)
                }
                
                // Game Cover Artwork
                Image(nsImage: icon)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: cardSize, height: cardSize)
                    .clipShape(RoundedRectangle(cornerRadius: cornerRad, style: .continuous))
                    .shadow(
                        color: isFocused ? NXTheme.primary.opacity(0.75) : Color.black.opacity(0.4),
                        radius: isFocused ? 22 : 8,
                        x: 0,
                        y: isFocused ? 10 : 4
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: cornerRad, style: .continuous)
                            .strokeBorder(
                                isFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.white.opacity(0.14)),
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
                    .padding(.top, 8)
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
                            .background(Color.black.opacity(0.72))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                            .overlay(
                                Capsule().stroke(Color.white.opacity(0.18), lineWidth: 0.5)
                            )
                    }
                    .padding(8)
                }
                .frame(width: cardSize, height: cardSize)
            }
            .scaleEffect(isFocused ? 1.05 : 1.0)
            .animation(.spring(response: 0.28, dampingFraction: 0.72), value: isFocused)
            
            // Game Name (Centered, high contrast, prominent)
            VStack(spacing: 2) {
                Text(game.name)
                    .font(.system(size: isFocused ? 15 : 12, weight: isFocused ? .bold : .medium, design: .rounded))
                    .foregroundColor(isFocused ? NXTheme.textPrimary : NXTheme.textSecondary)
                    .lineLimit(1)
                    .multilineTextAlignment(.center)
                
                Text(game.formattedSize)
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .foregroundColor(NXTheme.textMuted)
            }
            .frame(width: isFocused ? 230 : 170)
        }
        .contentShape(Rectangle())
    }
}
