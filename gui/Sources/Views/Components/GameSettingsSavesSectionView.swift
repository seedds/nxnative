import SwiftUI
import AppKit

public struct GameSettingsSavesSectionView: View {
    public let game: PortedGame
    @ObservedObject private var saveService = SaveSyncService.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared

    @State private var backupStatusText: String? = nil
    @State private var syncStatusText: String? = nil

    public init(game: PortedGame) {
        self.game = game
    }

    private var titleId: String {
        game.titleId ?? "UNKNOWN"
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            // MARK: - 1. iCloud Cross-Save Universal Management
            iCloudCrossSaveCard

            // MARK: - 2. Device Direct Transfer (Push / Pull)
            if !deviceService.connectedDevices.isEmpty {
                Divider().opacity(0.2)
                deviceSyncSection
            }
        }
        .padding(14)
        .background(NXTheme.glassSurface.opacity(0.6))
        .cornerRadius(12)
    }

    private var iCloudCrossSaveCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: "icloud.fill")
                        .foregroundColor(Color.blue)
                        .font(.system(size: 15))
                    Text("iCloud Cross-Save (Mac & iPhone/iPad)")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                }

                Spacer()

                Button("Local Backup") {
                    SoundEffectService.shared.play(.click)
                    if let backup = saveService.backupMacSave(titleId: titleId) {
                        backupStatusText = "Saved to: \(backup.lastPathComponent)"
                    } else {
                        backupStatusText = "No save data found to backup"
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)
            }

            // Status Banner
            HStack(spacing: 8) {
                Circle()
                    .fill(saveService.isICloudDriveAvailable ? Color.green : Color.orange)
                    .frame(width: 7, height: 7)

                VStack(alignment: .leading, spacing: 2) {
                    Text(saveService.isICloudDriveAvailable ? "iCloud Drive Sync Active" : "iCloud Drive Disabled on macOS")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.textPrimary)

                    Text("Saves are centralized in the nxnative_launcher folder with game cover art.")
                        .font(.system(size: 10))
                        .foregroundColor(NXTheme.textSecondary)
                }

                Spacer()

                Button(action: {
                    SoundEffectService.shared.play(.click)
                    let cover = GameCoverService.shared.coverImage(for: game)
                    saveService.revealCloudGameFolder(for: game, cover: cover)
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "folder.fill")
                        Text("Open Folder in Finder")
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)
                .help("Opens game folder inside iCloud Drive > nxnative_launcher with game cover art on the folder")
            }
            .padding(10)
            .background(NXTheme.glassSurfaceElevated.opacity(0.7))
            .cornerRadius(8)

            if let status = backupStatusText {
                Text(status)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
            }
        }
    }

    private var deviceSyncSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Direct Device Save Sync", systemImage: "arrow.triangle.2.circlepath")
                .font(.system(size: 12, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

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
                        _ = saveService.createSafetySnapshot(for: game, label: "pre_push")
                        saveService.pushSaveToDevice(
                            titleId: titleId,
                            bundleId: game.bundleId ?? "com.nxnative.\(titleId)",
                            device: dev
                        ) { success, _ in
                            syncStatusText = success ? "Save sent to \(dev.name)!" : "Failed to send save"
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.mini)

                    Button("Pull from Device") {
                        SoundEffectService.shared.play(.click)
                        _ = saveService.createSafetySnapshot(for: game, label: "pre_pull")
                        saveService.pullSaveFromDevice(
                            titleId: titleId,
                            bundleId: game.bundleId ?? "com.nxnative.\(titleId)",
                            device: dev
                        ) { success, _ in
                            syncStatusText = success ? "Save imported from \(dev.name)!" : "Failed to import save"
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.mini)
                }
                .padding(.vertical, 2)
            }
        }
    }
}
