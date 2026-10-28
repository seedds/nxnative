import SwiftUI
import AppKit

public struct ConsoleHeroView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var artworkStore = ArtworkStore.shared
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

            // MARK: - Dark Mode Linear Blend Overlay (Matches paint0_linear_31_76)
            LinearGradient(
                stops: [
                    .init(color: Color.clear, location: 0.0),
                    .init(color: Color.black.opacity(0.30), location: 0.40),
                    .init(color: Color.nxCanvas.opacity(0.75), location: 0.78),
                    .init(color: Color.nxCanvas, location: 1.0)
                ],
                startPoint: .top,
                endPoint: .bottom
            )

            LinearGradient(
                stops: [
                    .init(color: Color.black.opacity(0.70), location: 0.0),
                    .init(color: Color.clear, location: 0.75)
                ],
                startPoint: .leading,
                endPoint: .trailing
            )

            // MARK: - Game Metadata & Console Action Buttons
            VStack(alignment: .leading, spacing: 12) {
                // Console Runtime Status Indicators
                HStack(spacing: 8) {
                    statusPill(title: config.enable60FPS ? "60 FPS" : "30 FPS", active: config.enable60FPS)
                    statusPill(title: config.isDockedMode ? "Docked" : "Handheld", active: true)
                    if config.enableMetalFXUpscale {
                        statusPill(title: "MetalFX", active: true)
                    }
                    statusPill(title: game.platform == .macApp ? "macOS Native" : "iOS Port", active: false)
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
                            SoundEffectService.shared.play(.click)
                            viewModel.launchGame(game)
                        }) {
                            HStack(spacing: 8) {
                                if isLaunching {
                                    ProgressView()
                                        .controlSize(.small)
                                    Text("Launching...")
                                        .font(.system(size: 13, weight: .bold, design: .rounded))
                                } else {
                                    Image(systemName: "play.fill")
                                        .font(.system(size: 12))
                                    Text("Play Game")
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
                        // Send to Device Button for iOS IPAs
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            viewModel.gameForSettingsSheet = game
                        }) {
                            HStack(spacing: 8) {
                                Image(systemName: "arrow.up.forward.app")
                                    .font(.system(size: 13))
                                Text("Send to iPhone / iPad")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
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

                        // Reveal in Finder Button for iOS
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            NSWorkspace.shared.activateFileViewerSelecting([game.url])
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "folder.fill")
                                Text("Reveal .ipa")
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
                            Text("Options")
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
                            Text("Saves")
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
