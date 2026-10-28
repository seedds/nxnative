import SwiftUI
import AppKit

public struct CompatibilityView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredGames: [CompatibleGameItem] {
        viewModel.currentFilteredCompatibleGames()
    }
    
    public var body: some View {
        VStack(spacing: 0) {
            // MARK: - Toolbar & Header
            CompatibilityToolbarView(viewModel: viewModel)
                .padding(.horizontal, 18)
                .padding(.vertical, 12)
                .background(
                    ZStack {
                        NXTheme.canvasObsidian.opacity(0.8)
                        Rectangle().fill(.ultraThinMaterial)
                    }
                )
                .overlay(
                    Rectangle()
                        .frame(height: 1)
                        .foregroundColor(NXTheme.borderSubtle),
                    alignment: .bottom
                )
            
            // MARK: - Filter Bar
            CompatibilityFilterBarView(viewModel: viewModel)
            
            // MARK: - Main Games Scrollable View
            ScrollViewReader { proxy in
                ScrollView {
                    if filteredGames.isEmpty {
                        emptyFilterView
                    } else {
                        if viewModel.compatibilityDisplayMode == .list {
                            LazyVStack(spacing: 8) {
                                ForEach(Array(filteredGames.enumerated()), id: \.element.id) { index, game in
                                    let isFocused = viewModel.compatibilityFocusArea == .games && viewModel.focusedCompatibilityIndex == index
                                    CompatibleGameRow(
                                        game: game,
                                        isRowFocused: isFocused,
                                        focusedButton: isFocused ? viewModel.focusedCompatibilityButtonIndex : nil,
                                        onNotes: {
                                            if let url = game.githubUrl { NSWorkspace.shared.open(url) }
                                        }
                                    )
                                    .id(game.id)
                                    .onTapGesture {
                                        SoundEffectService.shared.play(.focus)
                                        viewModel.compatibilityFocusArea = .games
                                        viewModel.focusedCompatibilityIndex = index
                                    }
                                }
                            }
                            .padding(18)
                        } else {
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 180, maximum: 220), spacing: 18, alignment: .top)], spacing: 18) {
                                ForEach(Array(filteredGames.enumerated()), id: \.element.id) { index, game in
                                    let isFocused = viewModel.compatibilityFocusArea == .games && viewModel.focusedCompatibilityIndex == index
                                    CompatibleGameGridCard(
                                        item: game,
                                        isCardFocused: isFocused,
                                        focusedButton: isFocused ? viewModel.focusedCompatibilityButtonIndex : nil,
                                        onNotes: {
                                            if let url = game.githubUrl { NSWorkspace.shared.open(url) }
                                        }
                                    )
                                    .id(game.id)
                                    .onTapGesture {
                                        SoundEffectService.shared.play(.focus)
                                        viewModel.compatibilityFocusArea = .games
                                        viewModel.focusedCompatibilityIndex = index
                                    }
                                }
                            }
                            .padding(18)
                        }
                    }
                }
                .onChange(of: viewModel.focusedCompatibilityIndex) { newIndex in
                    if viewModel.compatibilityFocusArea == .games, newIndex >= 0, newIndex < filteredGames.count {
                        withAnimation(.easeInOut(duration: 0.2)) {
                            proxy.scrollTo(filteredGames[newIndex].id, anchor: .center)
                        }
                    }
                }
            }
        }
    }
    
    private var emptyFilterView: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 38))
                .foregroundColor(NXTheme.secondary.opacity(0.4))
            Text("No compatible games match your filter.")
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(NXTheme.textSecondary)
        }
        .frame(maxWidth: .infinity, minHeight: 300)
        .padding(40)
    }
}
