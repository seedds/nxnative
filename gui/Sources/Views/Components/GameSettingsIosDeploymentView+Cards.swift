import SwiftUI
import AppKit

// MARK: - Status Cards and Helpers for GameSettingsIosDeploymentView
extension GameSettingsIosDeploymentView {
    internal func formatSeconds(_ seconds: Int) -> String {
        let m = seconds / 60
        let s = seconds % 60
        return String(format: "%02d:%02d", m, s)
    }

    @ViewBuilder
    internal func activeDeploymentCard(deployment: DeploymentProgressInfo) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                ProgressView().controlSize(.small)
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
                Button(L10n.t("settings_modal.cancel_deployment")) {
                    SoundEffectService.shared.play(.click)
                    deviceService.cancelDeployment()
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)

                Spacer()

                Button(showDeployLogs ? "Hide Logs" : "View Live Logs") {
                    withAnimation(.easeInOut(duration: 0.2)) { showDeployLogs.toggle() }
                }
                .buttonStyle(NXPressableButtonStyle())
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

    @ViewBuilder
    internal func completedCard(deployment: DeploymentProgressInfo) -> some View {
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

            Button(L10n.t("common.ok")) {
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

    @ViewBuilder
    internal func failedCard(deployment: DeploymentProgressInfo) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.orange)
                    .font(.system(size: 16))

                Text(deployment.statusMessage)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(NXTheme.textPrimary)

                Spacer()

                Button(L10n.t("common.close")) {
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

            if deployment.errorCategory == .signatureVerificationFailed {
                HStack(spacing: 8) {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        deviceService.shareViaAirDrop(fileURL: game.url)
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "airdrop")
                            Text(L10n.t("settings_modal.airdrop_now"))
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
                            Text(L10n.t("settings_modal.open_ipa_folder"))
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
}
