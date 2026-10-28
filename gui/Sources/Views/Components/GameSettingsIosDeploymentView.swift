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
                devicesListSection
            }
        }
        .padding(14)
        .background(NXTheme.glassSurface.opacity(0.6))
        .cornerRadius(12)
    }

    private var headerBar: some View {
        HStack {
            Label("Install Port on iPhone / iPad", systemImage: "iphone.badge.play")
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
            .help("Send .ipa directly to iPhone via AirDrop")

            Button(action: {
                SoundEffectService.shared.play(.click)
                deviceService.revealInFinder(fileURL: game.url)
            }) {
                HStack(spacing: 4) {
                    Image(systemName: "folder")
                    Text("Show .ipa")
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .controlSize(.mini)
            .help("Show .ipa package in Finder for AltStore, TrollStore or Sideloadly")
        }
    }

    private var devicesListSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            if deviceService.connectedDevices.isEmpty {
                HStack(spacing: 8) {
                    if deviceService.isDetecting {
                        ProgressView().controlSize(.small)
                    }
                    Text("No devices found on local network or USB.")
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)

                    Spacer()

                    Button("Search Devices") {
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
                                .frame(width: 6, height: 6)

                            VStack(alignment: .leading, spacing: 1) {
                                Text(dev.name)
                                    .font(.system(size: 12, weight: .bold))
                                    .foregroundColor(NXTheme.textPrimary)
                                Text("\(dev.model) • iOS \(dev.osVersion)")
                                    .font(.system(size: 10))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                        }

                        Spacer()

                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            deviceService.shareViaAirDrop(fileURL: game.url)
                        }) {
                            HStack(spacing: 3) {
                                Image(systemName: "airdrop")
                                Text("AirDrop")
                            }
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)

                        Button("Install on \(dev.name)") {
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
}
