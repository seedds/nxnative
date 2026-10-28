import SwiftUI
import AppKit

public struct GameSettingsModsAndSavesView: View {
    public let game: PortedGame
    @ObservedObject private var modService = ModManagerService.shared
    @ObservedObject private var saveService = SaveSyncService.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared
    @State private var modsList: [GameModItem] = []
    @State private var backupStatusText: String? = nil
    @State private var syncStatusText: String? = nil
    @State private var isDeploying: Bool = false
    @State private var deployStatusText: String? = nil

    public init(game: PortedGame) {
        self.game = game
    }

    private var titleId: String {
        game.titleId ?? "UNKNOWN"
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            // MARK: - Mods Management Section
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

            // MARK: - iOS Port Deployment Section (AirDrop, Sideload & devicectl)
            if game.platform == .ios {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Label("Instalar Port no iPhone / iPad", systemImage: "arrow.up.forward.app")
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)

                        Spacer()

                        Button("Mostrar .ipa no Finder") {
                            SoundEffectService.shared.play(.click)
                            deviceService.revealInFinder(fileURL: game.url)
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.mini)
                        .help("Mostra o arquivo .ipa para compartilhar via AirDrop ou instalar com AltStore / TrollStore")
                    }

                    if let deployMsg = deployStatusText {
                        Text(deployMsg)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(deployMsg.contains("sucesso") ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.secondary)
                    }

                    if deviceService.connectedDevices.isEmpty {
                        HStack(spacing: 8) {
                            if deviceService.isDetecting {
                                ProgressView().controlSize(.small)
                            }
                            Text("Nenhum dispositivo encontrado na rede local / USB.")
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)

                            Spacer()

                            Button("Buscar Dispositivos") {
                                SoundEffectService.shared.play(.click)
                                deviceService.refreshDevicesAndTeams()
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.mini)
                        }
                        .padding(.vertical, 4)
                    } else {
                        ForEach(deviceService.connectedDevices) { dev in
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(dev.name)
                                        .font(.system(size: 12, weight: .bold))
                                        .foregroundColor(NXTheme.textPrimary)
                                    Text("\(dev.model) · iOS \(dev.osVersion) · Conectado")
                                        .font(.system(size: 10))
                                        .foregroundColor(NXTheme.textSecondary)
                                }

                                Spacer()

                                Button(isDeploying ? "Enviando..." : "Instalar no \(dev.name)") {
                                    SoundEffectService.shared.play(.launch)
                                    isDeploying = true
                                    deployStatusText = "Preparando e enviando para \(dev.name)..."
                                    deviceService.installApp(appBundleURL: game.url, on: dev) { success, msg in
                                        isDeploying = false
                                        deployStatusText = msg
                                        if success {
                                            SoundEffectService.shared.play(.click)
                                        } else {
                                            SoundEffectService.shared.play(.error)
                                        }
                                    }
                                }
                                .buttonStyle(NXPrimaryButtonStyle())
                                .controlSize(.small)
                                .disabled(isDeploying)
                            }
                            .padding(.vertical, 2)
                        }
                    }
                }
                .padding(14)
                .background(NXTheme.glassSurface.opacity(0.6))
                .cornerRadius(12)
            }

            // MARK: - Save Transfer & Backup Section
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label("Save Game Management & Device Sync", systemImage: "internaldrive.fill")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)

                    Spacer()

                    Button("Backup Save Now") {
                        SoundEffectService.shared.play(.click)
                        if let backup = saveService.backupMacSave(titleId: titleId) {
                            backupStatusText = "Backed up to \(backup.lastPathComponent)"
                        } else {
                            backupStatusText = "No save found to backup"
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.mini)
                }

                if let status = backupStatusText {
                    Text(status)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                }

                // Connected iPhone / iPad sync
                if !deviceService.connectedDevices.isEmpty {
                    Divider().opacity(0.2)

                    ForEach(deviceService.connectedDevices) { dev in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Sync with \(dev.name) (\(dev.model))")
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundColor(NXTheme.textPrimary)
                                if let text = syncStatusText {
                                    Text(text)
                                        .font(.system(size: 10))
                                        .foregroundColor(NXTheme.secondary)
                                }
                            }

                            Spacer()

                            Button("Push to Device") {
                                SoundEffectService.shared.play(.click)
                                saveService.pushSaveToDevice(
                                    titleId: titleId,
                                    bundleId: game.bundleId ?? "com.nxnative.\(titleId)",
                                    device: dev
                                ) { success, _ in
                                    syncStatusText = success ? "Transferred to \(dev.name)!" : "Sync failed"
                                }
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.mini)

                            Button("Pull from Device") {
                                SoundEffectService.shared.play(.click)
                                saveService.pullSaveFromDevice(
                                    titleId: titleId,
                                    bundleId: game.bundleId ?? "com.nxnative.\(titleId)",
                                    device: dev
                                ) { success, _ in
                                    syncStatusText = success ? "Imported from \(dev.name)!" : "Import failed"
                                }
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.mini)
                        }
                    }
                }
            }
            .padding(14)
            .background(NXTheme.glassSurface.opacity(0.6))
            .cornerRadius(12)
        }
        .onAppear {
            refreshMods()
            deviceService.refreshDevicesAndTeams()
        }
    }

    private func refreshMods() {
        modsList = modService.discoverMods(for: titleId)
    }
}
