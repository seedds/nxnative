import SwiftUI
import AppKit

public struct CompactConsoleActionSheetView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    public let onDelete: (PortedGame) -> Void
    public let onDismiss: () -> Void
    
    public init(
        game: PortedGame,
        viewModel: AppViewModel,
        onDelete: @escaping (PortedGame) -> Void,
        onDismiss: @escaping () -> Void
    ) {
        self.game = game
        self.viewModel = viewModel
        self.onDelete = onDelete
        self.onDismiss = onDismiss
    }
    
    private var isGamepadConnected: Bool {
        GameControllerService.shared.isGamepadConnected
    }
    
    public var body: some View {
        let profile = GameDetailsProfile.inspect(for: game)
        let config = viewModel.loadRuntimeConfig(for: game)
        let isLaunching = viewModel.launchingGameId == game.id
        let icon = GameCoverService.shared.coverImage(for: game) ?? NSWorkspace.shared.icon(forFile: game.url.path)
        
        ZStack {
            // Dark Backdrop
            Color.black.opacity(0.65)
                .ignoresSafeArea()
                .onTapGesture {
                    onDismiss()
                }
            
            // Central Compact Console Card
            VStack(spacing: 0) {
                // Header: Cover Art + Info + Close Button
                HStack(spacing: 16) {
                    Image(nsImage: icon)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 64, height: 64)
                        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 14, style: .continuous)
                                .strokeBorder(NXTheme.specularRim, lineWidth: 1)
                        )
                        .shadow(color: NXTheme.primary.opacity(0.4), radius: 8, x: 0, y: 4)
                    
                    VStack(alignment: .leading, spacing: 4) {
                        HStack(spacing: 8) {
                            Text(game.name)
                                .font(.system(size: 18, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                                .lineLimit(1)
                            
                            Text(game.platform == .macApp ? "Mac Native" : "iOS IPA")
                                .font(.system(size: 9, weight: .bold, design: .rounded))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(game.platform == .macApp ? NXTheme.primary.opacity(0.35) : NXTheme.secondary.opacity(0.35))
                                .foregroundColor(.white)
                                .clipShape(Capsule())
                        }
                        
                        HStack(spacing: 8) {
                            if let tid = game.titleId, !tid.isEmpty {
                                Text(tid)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundColor(NXTheme.textMuted)
                            }
                            
                            Text("•")
                                .foregroundColor(NXTheme.textMuted)
                            
                            Text(profile.formattedTotalSize)
                                .font(.system(size: 11, weight: .medium, design: .monospaced))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        
                        // Config Badges
                        HStack(spacing: 6) {
                            badgePill(text: config.enable60FPS ? "60 FPS" : "30 FPS")
                            badgePill(text: config.isDockedMode ? "Docked" : "Handheld")
                            if config.enableMetalFXUpscale {
                                badgePill(text: "MetalFX")
                            }
                            if !profile.dlcItems.isEmpty {
                                badgePill(text: "\(profile.dlcItems.count) DLCs")
                            }
                        }
                    }
                    
                    Spacer()
                    
                    Button(action: onDismiss) {
                        Image(systemName: "xmark")
                            .font(.system(size: 12, weight: .bold))
                            .foregroundColor(NXTheme.textSecondary)
                            .frame(width: 28, height: 28)
                            .background(NXTheme.glassSurfaceElevated.opacity(0.8))
                            .clipShape(Circle())
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 20)
                .padding(.top, 20)
                .padding(.bottom, 16)
                
                Divider()
                    .foregroundColor(NXTheme.borderSubtle)
                
                // Action Menu Items (Compact Console List)
                VStack(spacing: 8) {
                    if game.platform == .macApp {
                        // Option 0: Play Game
                        menuActionRow(
                            index: 0,
                            title: isLaunching ? viewModel.tr("actions.launching") : viewModel.tr("actions.play_game"),
                            subtitle: viewModel.tr("actions.play_subtitle"),
                            icon: isLaunching ? "hourglass" : "play.fill",
                            isPrimary: true,
                            isLoading: isLaunching
                        ) {
                            if !isLaunching {
                                viewModel.launchGame(game)
                                onDismiss()
                            }
                        }
                    } else {
                        // Option 0: Send to iPhone / iPad
                        menuActionRow(
                            index: 0,
                            title: viewModel.tr("actions.send_ios"),
                            subtitle: viewModel.tr("actions.send_ios_subtitle"),
                            icon: "arrow.up.forward.app",
                            isPrimary: true
                        ) {
                            onDismiss()
                            viewModel.gameForSettingsSheet = game
                        }
                    }
                    
                    // Option 1: Game Settings
                    menuActionRow(
                        index: 1,
                        title: viewModel.tr("actions.game_settings"),
                        subtitle: game.platform == .macApp ? viewModel.tr("actions.settings_mac_subtitle") : viewModel.tr("actions.settings_ios_subtitle"),
                        icon: "slider.horizontal.3",
                        isPrimary: false
                    ) {
                        onDismiss()
                        viewModel.gameForSettingsSheet = game
                    }
                    
                    if game.platform == .ios {
                        // Option 2: AirDrop for iOS
                        menuActionRow(
                            index: 2,
                            title: viewModel.tr("actions.share_airdrop"),
                            subtitle: viewModel.tr("actions.share_airdrop_subtitle"),
                            icon: "airdrop",
                            isPrimary: false
                        ) {
                            onDismiss()
                            DeviceDeploymentService.shared.shareViaAirDrop(fileURL: game.url)
                        }
                    } else {
                        // Option 2: Reveal App Files
                        menuActionRow(
                            index: 2,
                            title: viewModel.tr("actions.app_files"),
                            subtitle: viewModel.tr("actions.app_files_subtitle"),
                            icon: "folder.fill",
                            isPrimary: false
                        ) {
                            onDismiss()
                            viewModel.revealInFinder(url: game.url)
                        }
                    }
                    
                    // Option 3: Save Location
                    menuActionRow(
                        index: 3,
                        title: viewModel.tr("actions.save_location"),
                        subtitle: viewModel.tr("actions.save_location_subtitle"),
                        icon: "internaldrive.fill",
                        isPrimary: false
                    ) {
                        onDismiss()
                        viewModel.revealSaveFolder(profile.saveDirectory)
                    }
                    
                    if game.platform == .ios {
                        // Option 4: Reveal .ipa in Finder
                        menuActionRow(
                            index: 4,
                            title: viewModel.tr("actions.reveal_ipa"),
                            subtitle: viewModel.tr("actions.reveal_ipa_subtitle"),
                            icon: "folder.fill",
                            isPrimary: false
                        ) {
                            onDismiss()
                            viewModel.revealInFinder(url: game.url)
                        }
                    } else {
                        // Option 4: iCloud Cross-Save Folder in Finder
                        menuActionRow(
                            index: 4,
                            title: "iCloud Cross-Save Folder",
                            subtitle: "Open unified Mac & iOS cloud saves in Finder",
                            icon: "icloud.fill",
                            isPrimary: false
                        ) {
                            onDismiss()
                            let cover = GameCoverService.shared.coverImage(for: game)
                            SaveSyncService.shared.revealCloudGameFolder(for: game, cover: cover)
                        }
                    }
                    
                    // Option 5: Remove Game
                    menuActionRow(
                        index: 5,
                        title: viewModel.tr("actions.delete_game"),
                        subtitle: viewModel.tr("actions.delete_game_subtitle"),
                        icon: "trash.fill",
                        isPrimary: false,
                        isDestructive: true
                    ) {
                        onDismiss()
                        onDelete(game)
                    }
                }
                .padding(16)
                
                // Console Footer Controller Hints
                HStack(spacing: 16) {
                    controllerHint(button: isGamepadConnected && GameControllerService.shared.activeBrand == .playstation ? "✕" : "A", action: viewModel.tr("actions.select"))
                    controllerHint(button: isGamepadConnected && GameControllerService.shared.activeBrand == .playstation ? "○" : "B", action: viewModel.tr("actions.back"))
                    controllerHint(button: "↑ / ↓", action: "Navigate")
                    
                    Spacer()
                    
                    Text(viewModel.tr("console.banner_title"))
                        .font(.system(size: 9, weight: .bold, design: .monospaced))
                        .foregroundColor(NXTheme.textMuted)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
                .background(NXTheme.canvasObsidian.opacity(0.95))
            }
            .frame(width: 480)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .fill(NXTheme.canvasObsidian.opacity(0.92))
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 20, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 1.2)
            )
            .shadow(color: Color.black.opacity(0.8), radius: 30, x: 0, y: 15)
        }
    }
}

