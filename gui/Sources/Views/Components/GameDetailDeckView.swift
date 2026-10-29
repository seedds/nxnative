import SwiftUI
import AppKit

public struct GameDetailDeckView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    public let onDelete: (PortedGame) -> Void
    
    public init(game: PortedGame, viewModel: AppViewModel, onDelete: @escaping (PortedGame) -> Void) {
        self.game = game
        self.viewModel = viewModel
        self.onDelete = onDelete
    }
    
    public var body: some View {
        let profile = GameDetailsProfile.inspect(for: game)
        let config = viewModel.loadRuntimeConfig(for: game)
        let isLaunching = viewModel.launchingGameId == game.id
        let isDeckFocused = viewModel.isFocusOnDeck
        
        return GlassCard(padding: 20) {
            VStack(alignment: .leading, spacing: 16) {
                // Top Header Row: Title, TitleID, Platform, Action Buttons
                HStack(alignment: .center, spacing: 14) {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            Text(game.name)
                                .font(.system(size: 20, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            
                            Text(game.platform == .macApp ? "Mac Native" : "iOS IPA")
                                .font(.system(size: 10, weight: .bold, design: .rounded))
                                .padding(.horizontal, 7)
                                .padding(.vertical, 3)
                                .background(game.platform == .macApp ? NXTheme.primary.opacity(0.35) : NXTheme.secondary.opacity(0.35))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                        
                        if let tid = game.titleId, !tid.isEmpty {
                            Text("Title ID: \(tid)")
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundColor(NXTheme.textMuted)
                        }
                    }
                    
                    Spacer()
                    
                    // Quick Config Pills
                    HStack(spacing: 6) {
                        configPill(title: config.enable60FPS ? "60 FPS" : "30 FPS", active: config.enable60FPS)
                        configPill(title: config.isDockedMode ? "Docked" : "Handheld", active: true)
                        if config.enableMetalFXUpscale {
                            configPill(title: "MetalFX", active: true)
                        }
                    }
                }
                
                Divider().opacity(0.25)
                
                // Main Grid: Actions & Details
                LazyVGrid(columns: [GridItem(.flexible(minimum: 280)), GridItem(.flexible(minimum: 280))], spacing: 14) {
                    // Left Column: Play & Settings Actions
                    VStack(alignment: .leading, spacing: 12) {
                        // Play Button with anti-double-click loading state
                        if game.isPlayableLocally {
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                viewModel.launchGame(game)
                            }) {
                                HStack(spacing: 8) {
                                    if isLaunching {
                                        ProgressView()
                                            .controlSize(.small)
                                        Text(viewModel.tr("common.launching"))
                                            .font(.system(size: 13, weight: .bold, design: .rounded))
                                    } else {
                                        Image(systemName: "play.fill")
                                            .font(.system(size: 13))
                                        Text(viewModel.tr("common.play"))
                                            .font(.system(size: 13, weight: .bold, design: .rounded))
                                    }
                                }
                                .foregroundColor(.white)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 10)
                            }
                            .buttonStyle(NXPrimaryButtonStyle())
                            .disabled(isLaunching || viewModel.launchingGameId != nil)
                            .shadow(color: isDeckFocused ? NXTheme.primary.opacity(0.6) : Color.clear, radius: 8)
                        }
                        
                        HStack(spacing: 10) {
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                viewModel.gameForSettingsSheet = game
                            }) {
                                HStack(spacing: 6) {
                                    Image(systemName: "slider.horizontal.3")
                                    Text(viewModel.tr("deck.settings"))
                                }
                                .font(.system(size: 11, weight: .semibold, design: .rounded))
                                .frame(maxWidth: .infinity)
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.regular)
                            
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                viewModel.revealInFinder(url: game.url)
                            }) {
                                HStack(spacing: 6) {
                                    Image(systemName: "folder")
                                    Text(viewModel.tr("deck.app_files"))
                                }
                                .font(.system(size: 11, weight: .semibold, design: .rounded))
                                .frame(maxWidth: .infinity)
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.regular)
                            
                            Menu {
                                Button {
                                    viewModel.gameToRemove = game
                                    viewModel.showRemoveConfirm = true
                                } label: {
                                    Label(viewModel.tr("actions.remove_from_library"), systemImage: "eye.slash")
                                }
                                Divider()
                                Button(role: .destructive) {
                                    onDelete(game)
                                } label: {
                                    Label(viewModel.tr("actions.move_to_trash"), systemImage: "trash")
                                }
                            } label: {
                                Image(systemName: "trash")
                                    .font(.system(size: 11))
                                    .foregroundColor(Color(red: 0.9, green: 0.3, blue: 0.3))
                                    .padding(6)
                                    .background(NXTheme.glassSurfaceElevated)
                                    .cornerRadius(6)
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 6)
                                            .stroke(NXTheme.specularRim, lineWidth: 0.8)
                                    )
                            }
                            .menuStyle(.borderlessButton)
                            .menuIndicator(.hidden)
                            .fixedSize()
                            .help(viewModel.tr("actions.move_to_trash"))
                        }
                        
                        // Storage Size Info Card
                        VStack(alignment: .leading, spacing: 4) {
                            Text(viewModel.tr("deck.storage_footprint"))
                                .font(.system(size: 11, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textSecondary)
                            
                            HStack {
                                Text(viewModel.tr("deck.total_size"))
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.textMuted)
                                Spacer()
                                Text(profile.formattedTotalSize)
                                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                                    .foregroundColor(NXTheme.textPrimary)
                            }
                        }
                        .padding(10)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                        .cornerRadius(8)
                    }
                    
                    // Right Column: DLCs & Save Files Location
                    VStack(alignment: .leading, spacing: 12) {
                        // Save Files Location
                        VStack(alignment: .leading, spacing: 6) {
                            HStack {
                                Label(viewModel.tr("actions.save_location"), systemImage: "internaldrive")
                                    .font(.system(size: 11, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textSecondary)
                                Spacer()
                                Button(viewModel.tr("common.open_folder")) {
                                    SoundEffectService.shared.play(.click)
                                    viewModel.revealSaveFolder(profile.saveDirectory)
                                }
                                .buttonStyle(NXSecondaryButtonStyle())
                                .controlSize(.mini)
                            }
                            
                            Text(profile.saveDirectory.path)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(NXTheme.textPrimary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .padding(7)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(NXTheme.canvasObsidian.opacity(0.6))
                                .cornerRadius(6)
                        }
                        .padding(10)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                        .cornerRadius(8)
                        
                        // DLC Installed
                        VStack(alignment: .leading, spacing: 6) {
                            HStack {
                                Label(viewModel.tr("deck.dlc_installed"), systemImage: "shippingbox.fill")
                                    .font(.system(size: 11, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textSecondary)
                                Spacer()
                                Text(viewModel.tr("deck.found_count", profile.dlcItems.count))
                                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                                    .foregroundColor(profile.dlcItems.isEmpty ? NXTheme.textMuted : NXTheme.secondary)
                            }
                            
                            if profile.dlcItems.isEmpty {
                                Text(viewModel.tr("deck.base_game_only"))
                                    .font(.system(size: 10))
                                    .foregroundColor(NXTheme.textMuted)
                                    .padding(.vertical, 2)
                            } else {
                                ForEach(profile.dlcItems.prefix(2)) { dlc in
                                    HStack {
                                        Text("• \(dlc.name)")
                                            .font(.system(size: 10, weight: .medium))
                                            .foregroundColor(NXTheme.textPrimary)
                                            .lineLimit(1)
                                        Spacer()
                                        Text(dlc.formattedSize)
                                            .font(.system(size: 9, design: .monospaced))
                                            .foregroundColor(NXTheme.textSecondary)
                                    }
                                }
                            }
                        }
                        .padding(10)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                        .cornerRadius(8)
                    }
                }
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(isDeckFocused ? NXTheme.primary.opacity(0.7) : Color.clear, lineWidth: 1.5)
        )
    }
    
    private func configPill(title: String, active: Bool) -> some View {
        Text(title)
            .font(.system(size: 9, weight: .bold, design: .monospaced))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(active ? NXTheme.primary.opacity(0.35) : NXTheme.textMuted.opacity(0.3))
            .foregroundColor(.white)
            .clipShape(Capsule())
    }
}
