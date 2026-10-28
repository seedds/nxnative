import SwiftUI
import AppKit

public struct GameSettingsIosDeploymentView: View {
    public let game: PortedGame
    @ObservedObject internal var deviceService = DeviceDeploymentService.shared
    @State internal var showDeployLogs: Bool = false

    public init(game: PortedGame) {
        self.game = game
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            headerBar

            let deployment = deviceService.activeDeployment

            if deployment.isRunning {
                activeDeploymentCard(deployment: deployment)
            } else if deployment.phase == .completed {
                completedCard(deployment: deployment)
            } else if deployment.phase == .failed {
                failedCard(deployment: deployment)
            }

            if !deployment.isRunning {
                installSetupCard
            }
        }
        .padding(14)
        .background(NXTheme.glassSurface.opacity(0.6))
        .cornerRadius(12)
        .onAppear {
            if deviceService.connectedDevices.isEmpty || deviceService.signingTeams.isEmpty {
                deviceService.refreshDevicesAndTeams()
            }
        }
    }

    private var headerBar: some View {
        HStack {
            Label(L10n.t("settings_modal.ios_deploy_title"), systemImage: "iphone.badge.play")
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)

            Spacer()

            Button(action: {
                SoundEffectService.shared.play(.click)
                deviceService.shareViaAirDrop(fileURL: game.url)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: "airdrop")
                    Text(L10n.t("common.airdrop"))
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.mini)
            .help(L10n.t("settings_modal.airdrop_help"))

            Button(action: {
                SoundEffectService.shared.play(.click)
                deviceService.revealInFinder(fileURL: game.url)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: "folder")
                    Text(L10n.t("settings_modal.show_ipa"))
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.mini)
            .help(L10n.t("settings_modal.reveal_ipa_help"))
        }
    }

    private var installSetupCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Row 1: Install on (Device Selection)
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Image(systemName: "iphone.gen3")
                        .foregroundColor(NXTheme.primary)
                        .font(.system(size: 13))
                    Text(L10n.t("settings_modal.install_on_label"))
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(NXTheme.textPrimary)
                }

                Spacer()

                Button(action: {
                    SoundEffectService.shared.play(.click)
                    deviceService.refreshDevicesAndTeams()
                }) {
                    HStack(spacing: 3) {
                        if deviceService.isDetecting {
                            ProgressView().controlSize(.mini)
                        } else {
                            Image(systemName: "arrow.clockwise")
                        }
                        Text(L10n.t("settings_modal.refresh_label"))
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.mini)
                .disabled(deviceService.isDetecting)

                if deviceService.connectedDevices.isEmpty {
                    Text(L10n.t("settings_modal.no_devices"))
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                } else {
                    Picker("", selection: $deviceService.selectedDevice) {
                        ForEach(deviceService.connectedDevices) { dev in
                            Text("\(dev.name) (\(dev.model) • iOS \(dev.osVersion))")
                                .tag(Optional(dev))
                        }
                    }
                    .pickerStyle(MenuPickerStyle())
                    .frame(maxWidth: 240)

                    Circle()
                        .fill(Color(red: 0.13, green: 0.77, blue: 0.37))
                        .frame(width: 7, height: 7)
                }
            }

            Divider()
                .background(NXTheme.borderSubtle)

            // Row 2: Signing team Selection
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Image(systemName: "person.badge.shield.checkmark")
                        .foregroundColor(NXTheme.primary)
                        .font(.system(size: 13))
                    Text(L10n.t("settings_modal.signing_team_label"))
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(NXTheme.textPrimary)
                }

                Spacer()

                if deviceService.signingTeams.isEmpty {
                    Text(L10n.t("settings_modal.no_teams_found"))
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                } else {
                    Picker("", selection: $deviceService.selectedSigningTeam) {
                        ForEach(deviceService.signingTeams) { team in
                            Text(team.displayName)
                                .tag(Optional(team))
                        }
                    }
                    .pickerStyle(MenuPickerStyle())
                    .frame(maxWidth: 280)
                }
            }

            Divider()
                .background(NXTheme.borderSubtle)

            // Row 3: Contextual Notice (Certificate / Developer Mode advice)
            if let team = deviceService.selectedSigningTeam {
                if !team.hasCertificateInKeychain {
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: "info.circle.fill")
                            .foregroundColor(.orange)
                            .font(.system(size: 14))

                        VStack(alignment: .leading, spacing: 6) {
                            Text(L10n.t("settings_modal.team_no_cert_notice"))
                                .font(.system(size: 10.5))
                                .foregroundColor(NXTheme.textSecondary)
                                .lineSpacing(2)

                            HStack {
                                Text(L10n.t("settings_modal.developer_mode_tip"))
                                    .font(.system(size: 10, weight: .medium))
                                    .foregroundColor(.orange.opacity(0.95))

                                Spacer()

                                Button(action: {
                                    SoundEffectService.shared.play(.click)
                                    deviceService.openXcodeAccounts()
                                }) {
                                    HStack(spacing: 3) {
                                        Image(systemName: "hammer.fill")
                                        Text(L10n.t("settings_modal.open_xcode_btn"))
                                    }
                                }
                                .buttonStyle(NXSecondaryButtonStyle())
                                .controlSize(.mini)
                            }
                        }
                    }
                    .padding(10)
                    .background(Color.orange.opacity(0.1))
                    .overlay(
                        RoundedRectangle(cornerRadius: 8)
                            .strokeBorder(Color.orange.opacity(0.25), lineWidth: 1)
                    )
                    .cornerRadius(8)
                } else {
                    HStack(alignment: .center, spacing: 8) {
                        Image(systemName: "checkmark.shield.fill")
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                            .font(.system(size: 14))

                        VStack(alignment: .leading, spacing: 2) {
                            Text(L10n.t("settings_modal.team_has_cert_notice"))
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)

                            Text(L10n.t("settings_modal.developer_mode_tip"))
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.textSecondary.opacity(0.8))
                        }

                        Spacer()
                    }
                    .padding(9)
                    .background(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.08))
                    .overlay(
                        RoundedRectangle(cornerRadius: 8)
                            .strokeBorder(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.25), lineWidth: 1)
                    )
                    .cornerRadius(8)
                }
            }

            // Row 4: Action Button
            HStack {
                Spacer()

                Button(action: {
                    guard let dev = deviceService.selectedDevice else { return }
                    SoundEffectService.shared.play(.click)
                    deviceService.installApp(appBundleURL: game.url, on: dev, team: deviceService.selectedSigningTeam) { success, _ in
                        if success {
                            SoundEffectService.shared.play(.click)
                        } else {
                            SoundEffectService.shared.play(.error)
                        }
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.down.to.line.circle.fill")
                        Text(L10n.t("settings_modal.install_button", deviceService.selectedDevice?.name ?? "iOS"))
                            .fontWeight(.bold)
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 5)
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .disabled(deviceService.selectedDevice == nil || deviceService.isDetecting)

                Spacer()
            }
            .padding(.top, 4)
        }
        .padding(12)
        .background(NXTheme.glassSurfaceElevated.opacity(0.75))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(NXTheme.borderSubtle, lineWidth: 1)
        )
        .cornerRadius(10)
    }
}
