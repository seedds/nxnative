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
                
                // MARK: - Switch Dark Mode Console Showcase (Hero 16:9 + Square Tiles 1:1)
                if filteredGames.isEmpty {
                    emptyLibraryView
                } else {
                    SwitchConsoleLibraryView(viewModel: viewModel, games: filteredGames)
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
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(40)
    }
}
