import SwiftUI
import AppKit

public struct SquareGameCardView: View {
    public let game: PortedGame
    @ObservedObject public var viewModel: AppViewModel
    public let isFocused: Bool
    public let baseSize: CGFloat
    public let onSelect: () -> Void
    @ObservedObject private var artworkStore = ArtworkStore.shared
    @ObservedObject private var coverService = GameCoverService.shared

    public init(
        game: PortedGame,
        viewModel: AppViewModel,
        isFocused: Bool,
        baseSize: CGFloat = 170,
        onSelect: @escaping () -> Void
    ) {
        self.game = game
        self.viewModel = viewModel
        self.isFocused = isFocused
        self.baseSize = baseSize
        self.onSelect = onSelect
    }

    private var iconImage: NSImage {
        ArtworkStore.shared.loadSquareIcon(for: game)
    }

    public var body: some View {
        Button(action: onSelect) {
            ZStack(alignment: .bottomTrailing) {
                // MARK: - 1:1 Square Nintendo Switch Tile (Edge-to-Edge Full Bleed)
                Image(nsImage: iconImage)
                    .resizable()
                    .scaledToFill()
                    .frame(width: baseSize, height: baseSize, alignment: .top)
                    .clipped()
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))

                // Ambient gradient on bottom edge for subtle depth
                LinearGradient(
                    colors: [Color.clear, Color.black.opacity(0.35)],
                    startPoint: .center,
                    endPoint: .bottom
                )
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
            }
            .frame(width: baseSize, height: baseSize)
            // Liquid Glass micro-capsules placed outside so backdrop refraction renders natively
            .overlay(alignment: .bottomTrailing) {
                NXGlassContainer(spacing: 4) {
                    HStack(spacing: 4) {
                        Text(game.platform == .macApp ? "MAC" : "IOS")
                            .font(.system(size: 8, weight: .black, design: .rounded))
                            .foregroundColor(.white)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2.5)
                            .nxGlassBar(
                                in: Capsule(),
                                tint: (game.platform == .macApp ? NXTheme.primary : NXTheme.secondary).opacity(0.68),
                                fallbackFill: (game.platform == .macApp ? NXTheme.primary : NXTheme.secondary).opacity(0.85),
                                fallbackStroke: Color.white.opacity(0.22)
                            )

                        if SaveSyncService.shared.isCloudSyncActive(for: game) {
                            Image(systemName: "icloud.fill")
                                .font(.system(size: 8, weight: .bold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 2.5)
                                .nxGlassBar(
                                    in: Capsule(),
                                    tint: Color.blue.opacity(0.68),
                                    fallbackFill: Color.blue.opacity(0.85),
                                    fallbackStroke: Color.white.opacity(0.22)
                                )
                        }
                    }
                    .padding(8)
                }
            }
            // MARK: - Switch Dark Mode Glowing Focus Frame
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(
                        isFocused
                            ? AnyShapeStyle(LinearGradient(colors: [Color.white.opacity(0.95), NXTheme.primary], startPoint: .topLeading, endPoint: .bottomTrailing))
                            : AnyShapeStyle(Color.white.opacity(0.16)),
                        lineWidth: isFocused ? 3.5 : 1
                    )
            )
            .shadow(
                color: isFocused ? NXTheme.primary.opacity(0.65) : Color.black.opacity(0.35),
                radius: isFocused ? 8 : 4,
                x: 0,
                y: isFocused ? 2 : 2
            )
            .shadow(
                color: isFocused ? NXTheme.primary.opacity(0.35) : Color.clear,
                radius: isFocused ? 18 : 0,
                x: 0,
                y: isFocused ? 3 : 0
            )
            .scaleEffect(isFocused ? 1.08 : 1.0)
            .animation(.spring(response: 0.28, dampingFraction: 0.76), value: isFocused)
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
        .contextMenu {
            if game.platform == .macApp {
                Button {
                    viewModel.launchGame(game)
                } label: {
                    Label(viewModel.tr("common.play"), systemImage: "play.fill")
                }
            } else {
                Button {
                    viewModel.gameForSettingsSheet = game
                } label: {
                    Label(viewModel.tr("actions.send_ios"), systemImage: "arrow.up.forward.app")
                }
            }
            
            Button {
                viewModel.gameForSettingsSheet = game
            } label: {
                Label(viewModel.tr("actions.game_settings"), systemImage: "slider.horizontal.3")
            }
            
            Button {
                viewModel.revealInFinder(url: game.url)
            } label: {
                Label(viewModel.tr("actions.app_files"), systemImage: "folder")
            }
            
            Button {
                let profile = GameDetailsProfile.inspect(for: game)
                viewModel.revealSaveFolder(profile.saveDirectory)
            } label: {
                Label(viewModel.tr("actions.save_location"), systemImage: "internaldrive")
            }
            
            Divider()
            
            Button {
                viewModel.gameToRemove = game
                viewModel.showRemoveConfirm = true
            } label: {
                Label(viewModel.tr("actions.remove_from_library"), systemImage: "eye.slash")
            }
            
            Button(role: .destructive) {
                viewModel.gameToTrash = game
                viewModel.showTrashConfirm = true
            } label: {
                Label(viewModel.tr("actions.move_to_trash"), systemImage: "trash")
            }
        }
    }
}
