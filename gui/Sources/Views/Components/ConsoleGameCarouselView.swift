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
                HStack(spacing: 22) {
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
                .padding(.horizontal, 28)
                .padding(.vertical, 24)
            }
            .onChange(of: viewModel.focusedGameIndex) { newIndex in
                withAnimation(.spring(response: 0.28, dampingFraction: 0.8)) {
                    proxy.scrollTo(newIndex, anchor: .center)
                }
            }
        }
        .frame(height: 220)
    }
    
    private func gameCoverCard(game: PortedGame, index: Int, isFocused: Bool, isLaunching: Bool) -> some View {
        let icon = NSWorkspace.shared.icon(forFile: game.url.path)
        
        return VStack(spacing: 8) {
            ZStack(alignment: .top) {
                // Game Cover Artwork
                Image(nsImage: icon)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: isFocused ? 144 : 118, height: isFocused ? 144 : 118)
                    .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .shadow(
                        color: isFocused ? NXTheme.primary.opacity(0.65) : Color.black.opacity(0.35),
                        radius: isFocused ? 16 : 6,
                        x: 0,
                        y: isFocused ? 8 : 3
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                            .strokeBorder(
                                isFocused ? AnyShapeStyle(NXTheme.primaryGradient) : AnyShapeStyle(Color.white.opacity(0.12)),
                                lineWidth: isFocused ? 2.5 : 1
                            )
                    )
                
                // Active Launching / Playing Pill (as in reference image)
                if isLaunching {
                    HStack(spacing: 4) {
                        ProgressView()
                            .controlSize(.mini)
                        Text("Launching")
                            .font(.system(size: 9, weight: .bold, design: .rounded))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.black.opacity(0.75))
                    .clipShape(Capsule())
                    .overlay(
                        Capsule()
                            .strokeBorder(NXTheme.secondary.opacity(0.8), lineWidth: 0.8)
                    )
                    .padding(.top, 6)
                }
            }
            .scaleEffect(isFocused ? 1.05 : 1.0)
            .animation(.spring(response: 0.25, dampingFraction: 0.75), value: isFocused)
            
            // Game Name (Centered, high contrast)
            Text(game.name)
                .font(.system(size: isFocused ? 13 : 11, weight: isFocused ? .bold : .medium, design: .rounded))
                .foregroundColor(isFocused ? NXTheme.textPrimary : NXTheme.textSecondary)
                .lineLimit(1)
                .frame(width: isFocused ? 148 : 118)
                .multilineTextAlignment(.center)
        }
        .contentShape(Rectangle())
    }
}
