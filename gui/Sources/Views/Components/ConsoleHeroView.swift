import SwiftUI
import AppKit

public struct ConsoleHeroView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var artworkStore = ArtworkStore.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared
    @Environment(\.nxLiquidGlass) private var glassEnv
    public let height: CGFloat

    public init(game: PortedGame, viewModel: AppViewModel, height: CGFloat) {
        self.game = game
        self.viewModel = viewModel
        self.height = height
    }

    private var heroImage: NSImage? {
        ArtworkStore.shared.loadWidescreenHero(for: game)
    }

    private var config: GameRuntimeConfig {
        viewModel.loadRuntimeConfig(for: game)
    }

    private var isLaunching: Bool {
        viewModel.launchingGameId == game.id
    }

    public var body: some View {
        ZStack(alignment: .bottomLeading) {
            // MARK: - 16:9 Widescreen Promotional Artwork
            GeometryReader { geo in
                if let img = heroImage {
                    Image(nsImage: img)
                        .resizable()
                        .scaledToFill()
                        .frame(width: geo.size.width, height: height)
                        .clipped()
                } else {
                    // Procedural atmospheric gradient fallback
                    ZStack {
                        Color.nxCanvas.opacity(0.6)
                        RadialGradient(
                            gradient: Gradient(colors: [NXTheme.primary.opacity(0.34), Color.nxCanvas.opacity(0.85)]),
                            center: .topTrailing,
                            startRadius: 40,
                            endRadius: 600
                        )
                    }
                    .frame(width: geo.size.width, height: height)
                }
            }
            .allowsHitTesting(false)

            // MARK: - Dark Mode Linear Blend Overlay
            LinearGradient(
                stops: [
                    .init(color: Color.clear, location: 0.0),
                    .init(color: Color.black.opacity(0.18), location: 0.52),
                    .init(color: Color.nxCanvas.opacity(0.68), location: 0.84),
                    .init(color: Color.nxCanvas.opacity(glassEnv.isSupportedAndEnabled ? 0.88 : 1.0), location: 1.0)
                ],
                startPoint: .top,
                endPoint: .bottom
            )
            .allowsHitTesting(false)

            LinearGradient(
                stops: [
                    .init(color: Color.black.opacity(0.68), location: 0.0),
                    .init(color: Color.clear, location: 0.75)
                ],
                startPoint: .leading,
                endPoint: .trailing
            )
            .allowsHitTesting(false)

            // MARK: - Game Metadata & Liquid Glass Console Action Buttons
            VStack(alignment: .leading, spacing: 12) {
                // Console Runtime Status Indicators
                NXGlassContainer(spacing: 6) {
                    HStack(spacing: 8) {
                        if game.platform == .macApp {
                            statusPill(title: config.enable60FPS ? "60 FPS" : "30 FPS", active: config.enable60FPS)
                            statusPill(title: config.isDockedMode ? "Docked" : "Handheld", active: true)
                            if config.enableMetalFXUpscale {
                                statusPill(title: "MetalFX", active: true)
                            }
                            statusPill(title: "macOS Native", active: false)
                        } else {
                            statusPill(title: "iOS IPA Port", active: true)
                            statusPill(title: "Handheld", active: false)
                            if deviceService.activeDeployment.isRunning {
                                statusPill(title: "Deploying (\(Int(deviceService.activeDeployment.progressFraction * 100))%)", active: true)
                            } else {
                                statusPill(title: "iPhone & iPad", active: false)
                            }
                        }
                    }
                }

                // Clean Game Title
                Text(game.name)
                    .font(.system(size: 34, weight: .heavy, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                    .lineLimit(2)
                    .shadow(color: Color.black.opacity(0.8), radius: 10, x: 0, y: 3)

                // Interactive Liquid Glass Action Bar
                NXGlassContainer(spacing: 12) {
                    HStack(spacing: 12) {
                        if game.platform == .macApp {
                            // Play Button Pill for Mac Apps
                            Button(action: {
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
                                            .font(.system(size: 12))
                                        Text(viewModel.tr("common.play"))
                                            .font(.system(size: 14, weight: .bold, design: .rounded))
                                    }
                                }
                                .foregroundColor(.white)
                                .padding(.horizontal, 22)
                                .padding(.vertical, 10)
                                .contentShape(Capsule())
                                .nxGlassInteractive(
                                    in: Capsule(),
                                    tint: NXTheme.primary.opacity(glassEnv.appearance == .liquid ? 0.76 : 0.88),
                                    isSelected: true,
                                    isInteractive: !isLaunching,
                                    fallbackFill: NXTheme.primary,
                                    fallbackSelectedFill: NXTheme.primary
                                )
                            }
                            .buttonStyle(NXPressableButtonStyle())
                            .disabled(isLaunching)
                            .nxNoFocusRing()
                        } else {
                            // Send to Device Button for iOS IPAs
                            Button(action: {
                                SoundEffectService.shared.play(.click)
                                viewModel.gameForSettingsSheet = game
                            }) {
                                HStack(spacing: 8) {
                                    if deviceService.activeDeployment.isRunning {
                                        ProgressView()
                                            .controlSize(.small)
                                        Text("\(deviceService.activeDeployment.phase.rawValue) (\(Int(deviceService.activeDeployment.progressFraction * 100))%)")
                                            .font(.system(size: 13, weight: .bold, design: .rounded))
                                    } else {
                                        Image(systemName: "arrow.up.forward.app")
                                            .font(.system(size: 13))
                                        Text(viewModel.tr("console.send_ios"))
                                            .font(.system(size: 14, weight: .bold, design: .rounded))
                                    }
                                }
                                .foregroundColor(.white)
                                .padding(.horizontal, 20)
                                .padding(.vertical, 10)
                                .contentShape(Capsule())
                                .nxGlassInteractive(
                                    in: Capsule(),
                                    tint: NXTheme.primary.opacity(glassEnv.appearance == .liquid ? 0.76 : 0.88),
                                    isSelected: true,
                                    fallbackFill: NXTheme.primary,
                                    fallbackSelectedFill: NXTheme.primary
                                )
                            }
                            .buttonStyle(NXPressableButtonStyle())
                            .nxNoFocusRing()

                            // AirDrop Button for iOS
                            heroSecondaryButton(
                                icon: "airdrop",
                                title: viewModel.tr("common.airdrop")
                            ) {
                                SoundEffectService.shared.play(.click)
                                DeviceDeploymentService.shared.shareViaAirDrop(fileURL: game.url)
                            }
                            .help(viewModel.tr("settings_modal.airdrop_help"))

                            // Reveal in Finder Button for iOS
                            heroSecondaryButton(
                                icon: "folder.fill",
                                title: viewModel.tr("console.reveal_ipa")
                            ) {
                                SoundEffectService.shared.play(.click)
                                NSWorkspace.shared.activateFileViewerSelecting([game.url])
                            }
                        }

                        // Options Button
                        heroSecondaryButton(
                            icon: "slider.horizontal.3",
                            title: viewModel.tr("console.options")
                        ) {
                            SoundEffectService.shared.play(.click)
                            viewModel.gameForSettingsSheet = game
                        }

                        // Saves Folder Button
                        heroSecondaryButton(
                            icon: "folder.fill",
                            title: viewModel.tr("console.saves")
                        ) {
                            SoundEffectService.shared.play(.click)
                            let profile = GameDetailsProfile.inspect(for: game)
                            viewModel.revealSaveFolder(profile.saveDirectory)
                        }
                    }
                    .padding(.top, 4)
                }
            }
            .padding(.horizontal, 34)
            .padding(.bottom, 22)
        }
        .frame(height: height)
    }

    private func heroSecondaryButton(icon: String, title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                Text(title)
                    .font(.system(size: 12, weight: .semibold, design: .rounded))
            }
            .foregroundColor(NXTheme.textPrimary)
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .contentShape(Capsule())
            .nxGlassInteractive(
                in: Capsule(),
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.85)
            )
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }

    private func statusPill(title: String, active: Bool) -> some View {
        Text(title)
            .font(.system(size: 9, weight: .bold, design: .monospaced))
            .foregroundColor(.white)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .nxGlassBar(
                in: Capsule(),
                tint: active ? NXTheme.primary.opacity(0.46) : NXTheme.glassSurface.opacity(0.28),
                fallbackFill: active ? NXTheme.primary.opacity(0.35) : NXTheme.textMuted.opacity(0.24),
                fallbackStroke: active ? NXTheme.secondary.opacity(0.45) : Color.white.opacity(0.12)
            )
    }
}
