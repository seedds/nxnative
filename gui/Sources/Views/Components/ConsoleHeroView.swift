import SwiftUI
import AppKit

public struct ConsoleHeroView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var artworkStore = ArtworkStore.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared
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
                        Color.nxCanvas
                        RadialGradient(
                            gradient: Gradient(colors: [NXTheme.primary.opacity(0.3), Color.nxCanvas.opacity(0.9)]),
                            center: .topTrailing,
                            startRadius: 40,
                            endRadius: 600
                        )
                    }
                    .frame(width: geo.size.width, height: height)
                }
            }
            .allowsHitTesting(false)

            // MARK: - Dark Mode Linear Blend Overlay (Matches paint0_linear_31_76)
            LinearGradient(
                stops: [
                    .init(color: Color.clear, location: 0.0),
                    .init(color: Color.black.opacity(0.18), location: 0.52),
                    .init(color: Color.nxCanvas.opacity(0.70), location: 0.82),
                    .init(color: Color.nxCanvas, location: 1.0)
                ],
                startPoint: .top,
                endPoint: .bottom
            )
            .allowsHitTesting(false)

            LinearGradient(
                stops: [
                    .init(color: Color.black.opacity(0.70), location: 0.0),
                    .init(color: Color.clear, location: 0.75)
                ],
                startPoint: .leading,
                endPoint: .trailing
            )
            .allowsHitTesting(false)

            // MARK: - Game Metadata & Console Action Buttons
            VStack(alignment: .leading, spacing: 12) {
                // Console Runtime Status Indicators
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

                // Clean Game Title
                Text(game.name)
                    .font(.system(size: 34, weight: .heavy, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                    .lineLimit(2)
                    .shadow(color: Color.black.opacity(0.8), radius: 10, x: 0, y: 3)

                // Action Bar
                HStack(spacing: 14) {
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
                                    Text(viewModel.tr("common.play_game"))
                                        .font(.system(size: 14, weight: .bold, design: .rounded))
                                }
                            }
                            .foregroundColor(.white)
                            .padding(.horizontal, 22)
                            .padding(.vertical, 10)
                            .background(
                                Capsule()
                                    .fill(NXTheme.primaryGradient)
                            )
                            .overlay(
                                Capsule()
                                    .strokeBorder(Color.white.opacity(0.3), lineWidth: 1)
                            )
                            .shadow(color: NXTheme.primary.opacity(0.55), radius: 10, x: 0, y: 4)
                        }
                        .buttonStyle(.plain)
                        .disabled(isLaunching)
                    } else {
                        // Send to Device Button for iOS IPAs (With live progress if deployment running)
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
                            .background(
                                Capsule()
                                    .fill(NXTheme.primaryGradient)
                            )
                            .overlay(
                                Capsule()
                                    .strokeBorder(Color.white.opacity(0.3), lineWidth: 1)
                            )
                            .shadow(color: NXTheme.primary.opacity(0.55), radius: 10, x: 0, y: 4)
                        }
                        .buttonStyle(.plain)

                        // AirDrop Button for iOS
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            DeviceDeploymentService.shared.shareViaAirDrop(fileURL: game.url)
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "airdrop")
                                Text(viewModel.tr("common.airdrop"))
                                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                            }
                            .foregroundColor(NXTheme.textPrimary)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 10)
                            .background(
                                Capsule()
                                    .fill(NXTheme.glassSurfaceElevated.opacity(0.85))
                            )
                            .overlay(
                                Capsule()
                                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                            )
                        }
                        .buttonStyle(.plain)
                        .help(viewModel.tr("settings_modal.airdrop_help"))

                        // Reveal in Finder Button for iOS
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            NSWorkspace.shared.activateFileViewerSelecting([game.url])
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "folder.fill")
                                Text(viewModel.tr("console.reveal_ipa"))
                                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                            }
                            .foregroundColor(NXTheme.textPrimary)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 10)
                            .background(
                                Capsule()
                                    .fill(NXTheme.glassSurfaceElevated.opacity(0.85))
                            )
                            .overlay(
                                Capsule()
                                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                            )
                        }
                        .buttonStyle(.plain)
                    }

                    // Options Button
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        viewModel.gameForSettingsSheet = game
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "slider.horizontal.3")
                            Text(viewModel.tr("console.options"))
                                .font(.system(size: 12, weight: .semibold, design: .rounded))
                        }
                        .foregroundColor(NXTheme.textPrimary)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                        .background(
                            Capsule()
                                .fill(NXTheme.glassSurfaceElevated.opacity(0.85))
                        )
                        .overlay(
                            Capsule()
                                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                        )
                    }
                    .buttonStyle(.plain)

                    // Saves Folder Button
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        let profile = GameDetailsProfile.inspect(for: game)
                        viewModel.revealSaveFolder(profile.saveDirectory)
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "folder.fill")
                            Text(viewModel.tr("console.saves"))
                                .font(.system(size: 12, weight: .semibold, design: .rounded))
                        }
                        .foregroundColor(NXTheme.textSecondary)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                        .background(
                            Capsule()
                                .fill(NXTheme.glassSurfaceElevated.opacity(0.75))
                        )
                        .overlay(
                            Capsule()
                                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
                        )
                    }
                    .buttonStyle(.plain)
                }
                .padding(.top, 4)
            }
            .padding(.horizontal, 34)
            .padding(.bottom, 22)
        }
        .frame(height: height)
    }

    private func statusPill(title: String, active: Bool) -> some View {
        Text(title)
            .font(.system(size: 9, weight: .bold, design: .monospaced))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(active ? NXTheme.primary.opacity(0.35) : NXTheme.textMuted.opacity(0.2))
            .foregroundColor(.white)
            .clipShape(Capsule())
    }
}
