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
                    // Option 0: Play
                    menuActionRow(
                        index: 0,
                        title: isLaunching ? "Launching..." : "Play Game",
                        subtitle: "Start native execution on Apple Silicon",
                        icon: isLaunching ? "hourglass" : "play.fill",
                        isPrimary: true,
                        isLoading: isLaunching
                    ) {
                        if !isLaunching {
                            viewModel.launchGame(game)
                            onDismiss()
                        }
                    }
                    
                    // Option 1: Game Settings
                    menuActionRow(
                        index: 1,
                        title: "Game Settings",
                        subtitle: "Resolution, 60 FPS, Docked mode & MetalFX",
                        icon: "slider.horizontal.3",
                        isPrimary: false
                    ) {
                        onDismiss()
                        viewModel.gameForSettingsSheet = game
                    }
                    
                    // Option 2: Reveal App Files
                    menuActionRow(
                        index: 2,
                        title: "App Files",
                        subtitle: "Reveal .app bundle inside Finder",
                        icon: "folder.fill",
                        isPrimary: false
                    ) {
                        onDismiss()
                        viewModel.revealInFinder(url: game.url)
                    }
                    
                    // Option 3: Save Location
                    menuActionRow(
                        index: 3,
                        title: "Save Location",
                        subtitle: "Open game save data folder in Finder",
                        icon: "internaldrive.fill",
                        isPrimary: false
                    ) {
                        onDismiss()
                        viewModel.revealSaveFolder(profile.saveDirectory)
                    }
                    
                    // Option 4: DLC Packages
                    menuActionRow(
                        index: 4,
                        title: "DLC Packages (\(profile.dlcItems.count))",
                        subtitle: profile.dlcItems.isEmpty ? "No DLC packages detected in bundle" : "\(profile.dlcItems.count) content package(s) detected",
                        icon: "shippingbox.fill",
                        isPrimary: false
                    ) {
                        // Open game folder or inspect
                        onDismiss()
                        viewModel.revealInFinder(url: game.url)
                    }
                    
                    // Option 5: Remove Game
                    menuActionRow(
                        index: 5,
                        title: "Remove Game",
                        subtitle: "Delete compiled application from library",
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
                    controllerHint(button: isGamepadConnected && GameControllerService.shared.activeBrand == .playstation ? "✕" : "A", action: "Confirm")
                    controllerHint(button: isGamepadConnected && GameControllerService.shared.activeBrand == .playstation ? "○" : "B", action: "Back")
                    controllerHint(button: "↑ / ↓", action: "Navigate")
                    
                    Spacer()
                    
                    Text("NXNATIVE CONSOLE")
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

