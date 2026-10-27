import SwiftUI
import AppKit

public struct LibraryView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var controllerService = GameControllerService.shared
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var filteredGames: [PortedGame] {
        if viewModel.librarySearchText.trimmingCharacters(in: .whitespaces).isEmpty {
            return viewModel.libraryGames
        }
        return viewModel.libraryGames.filter {
            $0.name.localizedCaseInsensitiveContains(viewModel.librarySearchText)
        }
    }
    
    private var currentSelectedGame: PortedGame? {
        guard !filteredGames.isEmpty else { return nil }
        let index = max(0, min(viewModel.focusedGameIndex, filteredGames.count - 1))
        return filteredGames[index]
    }
    
    public var body: some View {
        ZStack {
            VStack(spacing: 0) {
                // MARK: - Library Toolbar (Liquid Glass, Search & Card Scale Slider)
                LibraryToolbarView(viewModel: viewModel)
                    .zIndex(10)
                
                // MARK: - Console Game Showcase & Detail Deck
                if filteredGames.isEmpty {
                    emptyLibraryView
                } else {
                    if controllerService.isGamepadConnected {
                        // Gamepad Mode: Clean Console View with Official Portrait Covers
                        VStack(spacing: 16) {
                            Spacer(minLength: 12)
                            
                            // Carousel with Portrait Console Box Art
                            ConsoleGameCarouselView(viewModel: viewModel, games: filteredGames)
                                .padding(.top, 24)
                            
                            // Clean Console Footer Action Hint (A / X to Play, Options for Menu)
                            gamepadFooterBar
                            
                            Spacer(minLength: 12)
                        }
                    } else {
                        // Mouse/Keyboard Mode: Scrollable with Detail Deck
                        ScrollView {
                            VStack(spacing: 24) {
                                // 1. Console Carousel (Portrait Official Cover Art on Focus)
                                ConsoleGameCarouselView(viewModel: viewModel, games: filteredGames)
                                    .padding(.top, 28)
                                
                                // 2. Focused Game Detail Deck (Play, DLC, Sizes, Saves, Settings)
                                if let selected = currentSelectedGame {
                                    GameDetailDeckView(game: selected, viewModel: viewModel) { target in
                                        viewModel.gameToDelete = target
                                        viewModel.showDeleteConfirm = true
                                    }
                                    .padding(.horizontal, 24)
                                }
                            }
                            .padding(.bottom, 24)
                        }
                    }
                }
                
                Spacer(minLength: 0)
                
                // MARK: - Game Controller / Keyboard Prompt Bar
                ControllerPromptBar(viewModel: viewModel)
            }
            
            // Compact Console Action Sheet (Overlay when Game Selected)
            if viewModel.isGameActionMenuPresented, let selected = currentSelectedGame {
                CompactConsoleActionSheetView(
                    game: selected,
                    viewModel: viewModel,
                    onDelete: { target in
                        viewModel.gameToDelete = target
                        viewModel.showDeleteConfirm = true
                    },
                    onDismiss: {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            viewModel.isGameActionMenuPresented = false
                        }
                    }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(100)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: viewModel.isGameActionMenuPresented)
        .sheet(item: $viewModel.gameForSettingsSheet) { game in
            GameSettingsSheetView(viewModel: viewModel, game: game)
        }
        .confirmationDialog(
            "Delete \(viewModel.gameToDelete?.name ?? "this game")?",
            isPresented: $viewModel.showDeleteConfirm,
            titleVisibility: .visible
        ) {
            Button("Delete File", role: .destructive) {
                if let g = viewModel.gameToDelete {
                    viewModel.deletePortedGame(g)
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This action will permanently delete the compiled game file from your storage.")
        }
    }
    
    private var gamepadFooterBar: some View {
        HStack(spacing: 14) {
            HStack(spacing: 6) {
                HStack(spacing: 2) {
                    Text(controllerService.activeBrand == .playstation ? "✕" : "A")
                        .font(.system(size: 10, weight: .black, design: .rounded))
                        .foregroundColor(.white)
                        .frame(width: 18, height: 18)
                        .background(Circle().fill(Color(red: 0.06, green: 0.49, blue: 0.06)))
                    
                    Text("/")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(NXTheme.textMuted)
                    
                    Text(controllerService.activeBrand == .playstation ? "□" : "X")
                        .font(.system(size: 10, weight: .black, design: .rounded))
                        .foregroundColor(.white)
                        .frame(width: 18, height: 18)
                        .background(Circle().fill(Color(red: 0.0, green: 0.47, blue: 0.84)))
                }
                
                Text("Play Game")
                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
            }
            
            Text("·")
                .foregroundColor(NXTheme.textMuted)
            
            HStack(spacing: 6) {
                Text(controllerService.activeBrand == .xbox ? "⧉" : (controllerService.activeBrand == .playstation ? "OPT" : "−"))
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2.5)
                    .background(Capsule().fill(Color(white: 0.22)))
                
                Text("Game Options")
                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                    .foregroundColor(.white)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(NXTheme.glassSurfaceElevated.opacity(0.75))
        .clipShape(Capsule())
        .overlay(
            Capsule().strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private var emptyLibraryView: some View {
        VStack(spacing: 14) {
            Image(systemName: "gamecontroller")
                .font(.system(size: 44))
                .foregroundColor(NXTheme.secondary.opacity(0.4))
            
            VStack(spacing: 4) {
                Text("No Ported Games Found")
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Games you convert (.app and .ipa) will automatically appear here.")
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Button("Convert a Game Now") {
                SoundEffectService.shared.play(.click)
                viewModel.selectedTab = 0
            }
            .buttonStyle(NXPrimaryButtonStyle())
            .controlSize(.regular)
        }
        .frame(maxWidth: .infinity, minHeight: 320)
        .padding(40)
    }
}

