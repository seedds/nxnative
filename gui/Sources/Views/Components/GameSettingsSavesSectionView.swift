import SwiftUI
import AppKit

public struct GameSettingsSavesSectionView: View {
    public let game: PortedGame
    @ObservedObject private var saveService = SaveSyncService.shared
    @ObservedObject private var deviceService = DeviceDeploymentService.shared

    @State private var backupStatusText: String? = nil
    @State private var syncStatusText: String? = nil
    @State private var isSyncing: Bool = false

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
                    Text(L10n.t("settings_modal.icloud_sync"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                }

                Spacer()

                Button(L10n.t("settings_modal.local_backup")) {
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

                    Text(L10n.t("settings_modal.saves_centralized"))
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
                        Text(L10n.t("common.reveal_finder"))
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)
                .help(L10n.t("settings_modal.icloud_folder_help"))
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
            Label(L10n.t("settings_modal.direct_sync_title"), systemImage: "arrow.triangle.2.circlepath")
                .font(.system(size: 12, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            Text(L10n.t("settings_modal.auto_sync_desc"))
                .font(.system(size: 10))
                .foregroundColor(NXTheme.textSecondary)

            ForEach(deviceService.connectedDevices) { dev in
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(L10n.t("settings_modal.sync_with_device", dev.name, dev.model))
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundColor(NXTheme.textPrimary)
                        if let text = syncStatusText {
                            Text(text)
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.secondary)
                        }
                    }

                    Spacer()

                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        isSyncing = true
                        syncStatusText = L10n.t("settings_modal.syncing_save")
                        saveService.autoSyncSave(
                            titleId: titleId,
                            bundleId: game.bundleId ?? "com.nxnative.\(titleId)",
                            device: dev,
                            game: game
                        ) { _, msg in
                            isSyncing = false
                            syncStatusText = msg
                        }
                    }) {
                        HStack(spacing: 5) {
                            if isSyncing {
                                ProgressView()
                                    .controlSize(.mini)
                            } else {
                                Image(systemName: "arrow.triangle.2.circlepath")
                            }
                            Text(isSyncing ? L10n.t("settings_modal.syncing_save") : L10n.t("settings_modal.sync_save"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .controlSize(.mini)
                    .disabled(isSyncing)
                }
                .padding(.vertical, 2)
            }
        }
    }
}
