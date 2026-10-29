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
            Group {
                // MARK: - Switch Dark Mode Console Showcase (Hero 16:9 + Square Tiles 1:1)
                if filteredGames.isEmpty {
                    emptyLibraryView
                } else {
                    SwitchConsoleLibraryView(viewModel: viewModel, games: filteredGames)
                }
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                // MARK: - Floating Liquid Glass Library Toolbar (Search & Card Scale Slider)
                LibraryToolbarView(viewModel: viewModel)
            }
            
            // Compact Console Action Sheet (Overlay when Game Selected)
            if viewModel.isGameActionMenuPresented, let selected = currentSelectedGame {
                CompactConsoleActionSheetView(
                    game: selected,
                    viewModel: viewModel,
                    onRemoveFromLibrary: { target in
                        viewModel.gameToRemove = target
                        viewModel.showRemoveConfirm = true
                    },
                    onDeleteToTrash: { target in
                        viewModel.gameToTrash = target
                        viewModel.showTrashConfirm = true
                    },
                    onDelete: { target in
                        viewModel.gameToTrash = target
                        viewModel.showTrashConfirm = true
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
                .environment(\.nxLiquidGlass, viewModel.liquidGlassEnvironment)
        }
        .confirmationDialog(
            viewModel.tr("library.remove_title", viewModel.gameToRemove?.name ?? "Game"),
            isPresented: $viewModel.showRemoveConfirm,
            titleVisibility: .visible
        ) {
            Button(viewModel.tr("library.remove_button")) {
                if let g = viewModel.gameToRemove {
                    viewModel.removeGameFromLibrary(g)
                }
            }
            Button(viewModel.tr("library.cancel"), role: .cancel) {}
        } message: {
            Text(viewModel.tr("library.remove_message"))
        }
        .confirmationDialog(
            viewModel.tr("library.trash_title", viewModel.gameToTrash?.name ?? "Game"),
            isPresented: $viewModel.showTrashConfirm,
            titleVisibility: .visible
        ) {
            Button(viewModel.tr("library.trash_button"), role: .destructive) {
                if let g = viewModel.gameToTrash {
                    viewModel.deleteGameToTrash(g)
                }
            }
            Button(viewModel.tr("library.cancel"), role: .cancel) {}
        } message: {
            Text(viewModel.tr("library.trash_message"))
        }
        .confirmationDialog(
            viewModel.tr("library.delete_title", viewModel.gameToDelete?.name ?? "Game"),
            isPresented: $viewModel.showDeleteConfirm,
            titleVisibility: .visible
        ) {
            Button(viewModel.tr("library.trash_button"), role: .destructive) {
                if let g = viewModel.gameToDelete {
                    viewModel.deleteGameToTrash(g)
                }
            }
            Button(viewModel.tr("library.cancel"), role: .cancel) {}
        } message: {
            Text(viewModel.tr("library.trash_message"))
        }
    }
    
    private var emptyLibraryView: some View {
        VStack(spacing: 14) {
            Image(systemName: "gamecontroller")
                .font(.system(size: 44))
                .foregroundColor(NXTheme.secondary.opacity(0.4))
            
            VStack(spacing: 4) {
                Text(viewModel.tr("library.empty_title"))
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("library.empty_subtitle"))
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Button(viewModel.tr("library.convert_now")) {
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
