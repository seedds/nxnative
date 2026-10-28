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
    @State private var showDeployLogs: Bool = false

    public init(game: PortedGame) {
        self.game = game
    }

    private var titleId: String {
        game.titleId ?? "UNKNOWN"
    }

    private func formatSeconds(_ seconds: Int) -> String {
        let m = seconds / 60
        let s = seconds % 60
        return String(format: "%02d:%02d", m, s)
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
                VStack(alignment: .leading, spacing: 12) {
                    HStack {
                        Label("Instalar Port no iPhone / iPad", systemImage: "iphone.badge.play")
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)

                        Spacer()

                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            deviceService.shareViaAirDrop(fileURL: game.url)
                        }) {
                            HStack(spacing: 4) {
                                Image(systemName: "airdrop")
                                Text("AirDrop")
                            }
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.mini)
                        .help("Enviar o .ipa diretamente para o iPhone via AirDrop")

                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            deviceService.revealInFinder(fileURL: game.url)
                        }) {
                            HStack(spacing: 4) {
                                Image(systemName: "folder")
                                Text("Mostrar .ipa")
                            }
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.mini)
                        .help("Mostrar o arquivo .ipa no Finder para AltStore, TrollStore ou Sideloadly")
                    }

                    let deployment = deviceService.activeDeployment

                    // 1. ACTIVE DEPLOYMENT PROGRESS CARD
                    if deployment.isRunning {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                ProgressView()
                                    .controlSize(.small)
                                Text(deployment.phase.rawValue)
                                    .font(.system(size: 12, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                HStack(spacing: 4) {
                                    Image(systemName: "stopwatch")
                                    Text(formatSeconds(deployment.elapsedSeconds))
                                }
                                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                                .foregroundColor(NXTheme.textSecondary)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 3)
                                .background(NXTheme.glassSurface.opacity(0.8))
                                .cornerRadius(6)
                            }

                            // Progress Bar & Percentage
                            VStack(alignment: .leading, spacing: 4) {
                                ProgressView(value: deployment.progressFraction, total: 1.0)
                                    .accentColor(NXTheme.primary)

                                HStack {
                                    Text(deployment.statusMessage)
                                        .font(.system(size: 11, weight: .medium))
                                        .foregroundColor(NXTheme.textPrimary)
                                    Spacer()
                                    Text("\(Int(deployment.progressFraction * 100))%")
                                        .font(.system(size: 11, weight: .bold, design: .rounded))
                                        .foregroundColor(NXTheme.primary)
                                }
                            }

                            if !deployment.detailMessage.isEmpty {
                                Text(deployment.detailMessage)
                                    .font(.system(size: 10))
                                    .foregroundColor(NXTheme.textSecondary)
                            }

                            HStack {
                                Button("Cancelar Envio") {
                                    SoundEffectService.shared.play(.click)
                                    deviceService.cancelDeployment()
                                }
                                .buttonStyle(NXSecondaryButtonStyle())
                                .controlSize(.mini)

                                Spacer()

                                Button(showDeployLogs ? "Ocultar Logs" : "Ver Logs em Tempo Real") {
                                    withAnimation(.easeInOut(duration: 0.2)) {
                                        showDeployLogs.toggle()
                                    }
                                }
                                .buttonStyle(.plain)
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.textSecondary)
                            }

                            if showDeployLogs && !deployment.logLines.isEmpty {
                                ScrollView {
                                    VStack(alignment: .leading, spacing: 2) {
                                        ForEach(Array(deployment.logLines.suffix(10).enumerated()), id: \.offset) { _, log in
                                            Text(log)
                                                .font(.system(size: 9, design: .monospaced))
                                                .foregroundColor(Color.green.opacity(0.85))
                                                .frame(maxWidth: .infinity, alignment: .leading)
                                        }
                                    }
                                    .padding(8)
                                }
                                .frame(height: 80)
                                .background(Color.black.opacity(0.45))
                                .cornerRadius(6)
                            }
                        }
                        .padding(12)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.8))
                        .overlay(
                            RoundedRectangle(cornerRadius: 10)
                                .strokeBorder(NXTheme.primary.opacity(0.35), lineWidth: 1)
                        )
                        .cornerRadius(10)
                    }
                    // 2. SUCCESS CARD
                    else if deployment.phase == .completed {
                        HStack(spacing: 10) {
                            Image(systemName: "checkmark.circle.fill")
                                .font(.system(size: 22))
                                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))

                            VStack(alignment: .leading, spacing: 2) {
                                Text(deployment.statusMessage)
                                    .font(.system(size: 12, weight: .bold))
                                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                                Text(deployment.detailMessage)
                                    .font(.system(size: 10))
                                    .foregroundColor(NXTheme.textSecondary)
                            }

                            Spacer()

                            Button("OK") {
                                SoundEffectService.shared.play(.click)
                                deviceService.dismissDeploymentStatus()
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                            .controlSize(.mini)
                        }
                        .padding(10)
                        .background(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.12))
                        .cornerRadius(8)
                    }
                    // 3. FAILED DEPLOYMENT CARD
                    else if deployment.phase == .failed {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack(spacing: 8) {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundColor(.orange)
                                    .font(.system(size: 16))

                                Text(deployment.statusMessage)
                                    .font(.system(size: 12, weight: .bold))
                                    .foregroundColor(NXTheme.textPrimary)

                                Spacer()

                                Button("Fechar") {
                                    SoundEffectService.shared.play(.click)
                                    deviceService.dismissDeploymentStatus()
                                }
                                .buttonStyle(NXSecondaryButtonStyle())
                                .controlSize(.mini)
                            }

                            Text(deployment.detailMessage)
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)

                            // Specific actions for signature rejection
                            if deployment.errorCategory == .signatureVerificationFailed {
                                HStack(spacing: 8) {
                                    Button(action: {
                                        SoundEffectService.shared.play(.click)
                                        deviceService.shareViaAirDrop(fileURL: game.url)
                                    }) {
                                        HStack(spacing: 4) {
                                            Image(systemName: "airdrop")
                                            Text("Enviar via AirDrop Agora")
                                        }
                                    }
                                    .buttonStyle(NXPrimaryButtonStyle())
                                    .controlSize(.small)

                                    Button(action: {
                                        SoundEffectService.shared.play(.click)
                                        deviceService.revealInFinder(fileURL: game.url)
                                    }) {
                                        HStack(spacing: 4) {
                                            Image(systemName: "folder")
                                            Text("Abrir Pasta do .ipa")
                                        }
                                    }
                                    .buttonStyle(NXSecondaryButtonStyle())
                                    .controlSize(.small)
                                }
                                .padding(.top, 4)
                            }
                        }
                        .padding(12)
                        .background(Color.orange.opacity(0.12))
                        .overlay(
                            RoundedRectangle(cornerRadius: 8)
                                .strokeBorder(Color.orange.opacity(0.3), lineWidth: 1)
                        )
                        .cornerRadius(8)
                    }

                    // 4. DEVICE LIST & ACTIONS (when idle)
                    if !deployment.isRunning {
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
                                    HStack(spacing: 6) {
                                        Circle()
                                            .fill(Color(red: 0.13, green: 0.77, blue: 0.37))
                                            .frame(width: 8, height: 8)

                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(dev.name)
                                                .font(.system(size: 12, weight: .bold))
                                                .foregroundColor(NXTheme.textPrimary)
                                            Text("\(dev.model) · iOS \(dev.osVersion) · Conectado")
                                                .font(.system(size: 10))
                                                .foregroundColor(NXTheme.textSecondary)
                                        }
                                    }

                                    Spacer()

                                    Button(action: {
                                        SoundEffectService.shared.play(.click)
                                        deviceService.shareViaAirDrop(fileURL: game.url)
                                    }) {
                                        HStack(spacing: 4) {
                                            Image(systemName: "airdrop")
                                            Text("AirDrop")
                                        }
                                    }
                                    .buttonStyle(NXSecondaryButtonStyle())
                                    .controlSize(.small)

                                    Button("Instalar no \(dev.name)") {
                                        SoundEffectService.shared.play(.launch)
                                        deviceService.installApp(appBundleURL: game.url, on: dev) { success, _ in
                                            if success {
                                                SoundEffectService.shared.play(.click)
                                            } else {
                                                SoundEffectService.shared.play(.error)
                                            }
                                        }
                                    }
                                    .buttonStyle(NXPrimaryButtonStyle())
                                    .controlSize(.small)
                                }
                                .padding(.vertical, 4)
                            }
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
