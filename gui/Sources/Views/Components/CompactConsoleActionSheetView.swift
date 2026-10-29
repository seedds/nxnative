import SwiftUI
import AppKit

public struct CompactConsoleActionSheetView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    public let onRemoveFromLibrary: ((PortedGame) -> Void)?
    public let onDeleteToTrash: ((PortedGame) -> Void)?
    public let onDelete: (PortedGame) -> Void
    public let onDismiss: () -> Void
    
    @Environment(\.nxLiquidGlass) internal var glassEnv
    @Environment(\.accessibilityReduceTransparency) internal var reduceTransparency
    @Namespace internal var actionRowNamespace
    
    public init(
        game: PortedGame,
        viewModel: AppViewModel,
        onRemoveFromLibrary: ((PortedGame) -> Void)? = nil,
        onDeleteToTrash: ((PortedGame) -> Void)? = nil,
        onDelete: @escaping (PortedGame) -> Void = { _ in },
        onDismiss: @escaping () -> Void
    ) {
        self.game = game
        self.viewModel = viewModel
        self.onRemoveFromLibrary = onRemoveFromLibrary
        self.onDeleteToTrash = onDeleteToTrash
        self.onDelete = onDelete
        self.onDismiss = onDismiss
    }
    
    private var isGamepadConnected: Bool {
        GameControllerService.shared.isGamepadConnected
    }
    
    internal var useLiquidGlass: Bool {
        glassEnv.isSupportedAndEnabled && !reduceTransparency
    }
    
    public var body: some View {
        let profile = GameDetailsProfile.inspect(for: game)
        let config = viewModel.loadRuntimeConfig(for: game)
        let isLaunching = viewModel.launchingGameId == game.id
        let icon = GameCoverService.shared.coverImage(for: game) ?? NSWorkspace.shared.icon(forFile: game.url.path)
        let backdropOpacity: Double = useLiquidGlass
            ? (glassEnv.appearance == .liquid ? 0.38 : 0.52)
            : 0.65
        
        ZStack {
            // Optical Backdrop Veil
            Color.black.opacity(backdropOpacity)
                .ignoresSafeArea()
                .onTapGesture {
                    onDismiss()
                }
            
            // Central Compact Console Card (Concentric: outer R=24, padding=12, rows R=12)
            NXGlassContainer(spacing: 8) {
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
                                .foregroundColor(NXTheme.textPrimary)
                                .frame(width: 30, height: 30)
                                .nxGlassInteractive(in: Circle())
                        }
                        .buttonStyle(NXPressableButtonStyle())
                        .nxNoFocusRing()
                    }
                    .padding(.horizontal, 20)
                    .padding(.top, 20)
                    .padding(.bottom, 14)
                    
                    Divider()
                        .opacity(useLiquidGlass ? 0.16 : 0.45)
                    
                    // Action Menu Items (Compact Console List)
                    VStack(spacing: 6) {
                        if game.platform == .macApp {
                            // Option 0: Play
                            menuActionRow(
                                index: 0,
                                title: isLaunching ? viewModel.tr("actions.launching") : viewModel.tr("common.play"),
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
                        
                        // Option 5: Remove from Library (Keep files on disk)
                        menuActionRow(
                            index: 5,
                            title: viewModel.tr("actions.remove_from_library"),
                            subtitle: viewModel.tr("actions.remove_from_library_subtitle"),
                            icon: "eye.slash",
                            isPrimary: false
                        ) {
                            onDismiss()
                            if let onRemove = onRemoveFromLibrary {
                                onRemove(game)
                            } else {
                                viewModel.gameToRemove = game
                                viewModel.showRemoveConfirm = true
                            }
                        }
                        
                        // Option 6: Move to macOS Trash
                        menuActionRow(
                            index: 6,
                            title: viewModel.tr("actions.move_to_trash"),
                            subtitle: viewModel.tr("actions.move_to_trash_subtitle"),
                            icon: "trash.fill",
                            isPrimary: false,
                            isDestructive: true
                        ) {
                            onDismiss()
                            if let onTrash = onDeleteToTrash {
                                onTrash(game)
                            } else {
                                onDelete(game)
                            }
                        }
                    }
                    .background {
                        NXLiquidSlidingPill(
                            in: RoundedRectangle(cornerRadius: 12, style: .continuous),
                            tint: viewModel.gameActionFocusedIndex == 6
                                ? Color(red: 0.92, green: 0.22, blue: 0.22).opacity(0.62)
                                : NXTheme.primary.opacity(glassEnv.appearance == .liquid ? 0.56 : 0.72),
                            fallbackFill: viewModel.gameActionFocusedIndex == 6
                                ? AnyShapeStyle(Color.red.opacity(0.24))
                                : AnyShapeStyle(NXTheme.glassSurfaceElevated.opacity(0.95))
                        )
                        .matchedGeometryEffect(
                            id: viewModel.gameActionFocusedIndex,
                            in: actionRowNamespace,
                            properties: .frame,
                            anchor: .center,
                            isSource: false
                        )
                        .animation(.spring(response: 0.28, dampingFraction: 0.78), value: viewModel.gameActionFocusedIndex)
                    }
                    .padding(12)
                    
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
                    .background(
                        useLiquidGlass
                            ? Color.black.opacity(glassEnv.appearance == .liquid ? 0.18 : 0.28)
                            : NXTheme.canvasObsidian.opacity(0.95)
                    )
                }
                .frame(width: 488)
                .clipShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
                .nxGlassCard(
                    cornerRadius: 24,
                    tint: useLiquidGlass
                        ? NXTheme.canvasObsidian.opacity(glassEnv.appearance == .liquid ? 0.36 : 0.62)
                        : nil
                )
                .shadow(color: Color.black.opacity(useLiquidGlass ? 0.55 : 0.8), radius: 30, x: 0, y: 15)
            }
        }
    }
}

