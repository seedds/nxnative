import SwiftUI
import AppKit

public struct GameSettingsModsAndSavesView: View {
    public let game: PortedGame
    @ObservedObject private var modService = ModManagerService.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared
    @State private var modsList: [GameModItem] = []

    public init(game: PortedGame) {
        self.game = game
    }

    private var titleId: String {
        game.titleId ?? "UNKNOWN"
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if game.platform == .ios {
                GameSettingsIosDeploymentView(game: game)
            }
            modsSection
            GameSettingsSavesSectionView(game: game)
        }
        .onAppear {
            refreshMods()
            deviceService.refreshDevicesAndTeams()
        }
    }

    // MARK: - Mods Management Section
    private var modsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("Game Mods (LayeredFS)", systemImage: "puzzlepiece.extension.fill")
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)

                Spacer()

                Button("Open Mods Folder") {
                    SoundEffectService.shared.play(.click)
                    modService.openModsFolder(for: titleId)
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)
            }

            if modsList.isEmpty {
                Text("No mods found. Place LayeredFS mod folders (romfs / exefs) in the game mods directory.")
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                    .cornerRadius(8)
            } else {
                VStack(spacing: 6) {
                    ForEach(modsList) { mod in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(mod.name)
                                    .font(.system(size: 12, weight: .semibold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)
                                if mod.hasCodePatches {
                                    Text("Executable code patch detected")
                                        .font(.system(size: 10))
                                        .foregroundColor(NXTheme.tertiary)
                                }
                            }
                            Spacer()
                            Toggle("", isOn: Binding(
                                get: { mod.isEnabled },
                                set: { _ in
                                    _ = modService.toggleMod(mod, in: titleId)
                                    refreshMods()
                                }
                            ))
                            .toggleStyle(.switch)
                            .labelsHidden()
                        }
                        .padding(8)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.4))
                        .cornerRadius(8)
                    }
                }
            }
        }
        .padding(14)
        .background(NXTheme.glassSurface.opacity(0.6))
        .cornerRadius(12)
    }

    private func refreshMods() {
        modsList = modService.discoverMods(for: titleId)
    }
}
