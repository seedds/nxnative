import SwiftUI
import AppKit

public struct LauncherUpdateSheet: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ZStack {
            NXTheme.canvasObsidian.ignoresSafeArea()
            
            VStack(alignment: .leading, spacing: 16) {
                // MARK: - Header
                HStack(spacing: 12) {
                    ZStack {
                        Circle()
                            .fill(NXTheme.primaryGradient)
                            .frame(width: 42, height: 42)
                        Image(systemName: "arrow.triangle.2.circlepath")
                            .font(.system(size: 18, weight: .bold))
                            .foregroundColor(.white)
                    }
                    
                    VStack(alignment: .leading, spacing: 2) {
                        Text("NxNative Launcher Software Update")
                            .font(.system(size: 16, weight: .bold, design: .rounded))
                            .foregroundColor(.white)
                        Text("GitHub Repository: github.com/rennan101/nxnative_Launcher")
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    
                    Spacer()
                }
                
                Divider().opacity(0.2)
                
                // MARK: - Main Status Content
                if viewModel.isCheckingUpdate {
                    HStack(spacing: 12) {
                        ProgressView()
                            .controlSize(.regular)
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Checking GitHub for new versions...")
                                .font(.system(size: 13, weight: .semibold))
                                .foregroundColor(.white)
                            Text("Comparing local commit with origin/main on GitHub...")
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        Spacer()
                    }
                    .padding(16)
                    .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                    .cornerRadius(10)
                } else if viewModel.isApplyingUpdate {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack(spacing: 10) {
                            ProgressView()
                                .controlSize(.small)
                            Text("Downloading & Installing New Launcher Version Automatically...")
                                .font(.system(size: 13, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.secondary)
                            Spacer()
                        }
                        
                        updateLogsConsole
                    }
                } else if viewModel.updateCompletedReadyToRelaunch {
                    VStack(alignment: .leading, spacing: 12) {
                        HStack(spacing: 10) {
                            Image(systemName: "checkmark.seal.fill")
                                .font(.system(size: 22))
                                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Update Installed Successfully!")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                                Text(viewModel.updateSuccessBanner ?? "The new NxNative_Launcher.app bundle has been compiled and installed.")
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                            Spacer()
                        }
                        .padding(12)
                        .background(Color.green.opacity(0.14))
                        .cornerRadius(10)
                        
                        if !viewModel.updateInstallLogs.isEmpty {
                            updateLogsConsole
                        }
                    }
                } else if let err = viewModel.updateErrorBanner {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack(spacing: 10) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .font(.system(size: 20))
                                .foregroundColor(.orange)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Update Encountered an Issue")
                                    .font(.system(size: 13, weight: .bold))
                                    .foregroundColor(.white)
                                Text(err)
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                            Spacer()
                        }
                        .padding(12)
                        .background(Color.orange.opacity(0.14))
                        .cornerRadius(10)
                        
                        if !viewModel.updateInstallLogs.isEmpty {
                            updateLogsConsole
                        }
                    }
                } else if let info = viewModel.updateInfo {
                    if info.hasUpdate {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("New Version Available on GitHub!", systemImage: "sparkles")
                                    .font(.system(size: 13, weight: .bold, design: .rounded))
                                    .foregroundColor(NXTheme.secondary)
                                Spacer()
                                Text("[\(info.localCommit)] → [\(info.remoteCommit)]")
                                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                                    .foregroundColor(.white)
                            }
                            
                            ScrollView {
                                VStack(alignment: .leading, spacing: 4) {
                                    ForEach(info.commitMessages, id: \.self) { msg in
                                        Text("• \(msg)")
                                            .font(.system(size: 11, design: .monospaced))
                                            .foregroundColor(NXTheme.textSecondary)
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                            }
                            .frame(maxHeight: 120)
                            .padding(10)
                            .background(NXTheme.glassSurfaceElevated)
                            .cornerRadius(8)
                        }
                    } else {
                        HStack(spacing: 12) {
                            Image(systemName: "checkmark.circle.fill")
                                .font(.system(size: 26))
                                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                            
                            VStack(alignment: .leading, spacing: 3) {
                                Text("NxNative Launcher is Up to Date!")
                                    .font(.system(size: 14, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                                Text("You are running the latest version from GitHub (Commit [\(info.localCommit)]).")
                                    .font(.system(size: 11.5))
                                    .foregroundColor(NXTheme.textSecondary)
                            }
                            Spacer()
                        }
                        .padding(14)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.6))
                        .cornerRadius(10)
                    }
                } else {
                    HStack(spacing: 10) {
                        Image(systemName: "wifi.exclamationmark")
                            .font(.system(size: 20))
                            .foregroundColor(.orange)
                        Text("Could not reach GitHub or locate local git repository (\(viewModel.nxnativePath)).")
                            .font(.system(size: 11.5))
                            .foregroundColor(NXTheme.textSecondary)
                        Spacer()
                    }
                    .padding(14)
                    .background(NXTheme.glassSurfaceElevated.opacity(0.5))
                    .cornerRadius(10)
                }
                
                Divider().opacity(0.2)
                
                // MARK: - Footer Buttons
                HStack(spacing: 10) {
                    if !viewModel.isCheckingUpdate && !viewModel.isApplyingUpdate {
                        Button(action: {
                            viewModel.openUpdateCheckerAndAutoInstall()
                        }) {
                            HStack(spacing: 5) {
                                Image(systemName: "arrow.clockwise")
                                Text("Re-check GitHub")
                            }
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        if let info = viewModel.updateInfo, !info.hasUpdate, !viewModel.updateCompletedReadyToRelaunch {
                            Button(action: {
                                viewModel.applyUpdate(autoRelaunch: false)
                            }) {
                                HStack(spacing: 5) {
                                    Image(systemName: "hammer.fill")
                                    Text("Force Rebuild & Reinstall Launcher")
                                }
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                        }
                    }
                    
                    Spacer()
                    
                    if viewModel.updateCompletedReadyToRelaunch {
                        Button(action: {
                            viewModel.relaunchUpdatedLauncher()
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "bolt.fill")
                                Text("Relaunch Launcher Now")
                            }
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    } else if let info = viewModel.updateInfo, info.hasUpdate, !viewModel.isApplyingUpdate {
                        Button(action: {
                            viewModel.applyUpdate(autoRelaunch: true)
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "arrow.down.circle.fill")
                                Text("Install Update & Relaunch")
                            }
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    }
                    
                    if !viewModel.isApplyingUpdate {
                        Button("Close") {
                            dismiss()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                    }
                }
            }
            .padding(22)
        }
        .frame(width: 540)
        .fixedSize(horizontal: false, vertical: true)
    }
    
    private var updateLogsConsole: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 3) {
                    ForEach(Array(viewModel.updateInstallLogs.enumerated()), id: \.offset) { idx, line in
                        Text(line)
                            .font(.system(size: 10.5, design: .monospaced))
                            .foregroundColor(line.hasPrefix("==>") ? NXTheme.secondary : NXTheme.textSecondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(idx)
                    }
                }
                .padding(10)
            }
            .frame(height: 150)
            .background(Color.black.opacity(0.45))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.7)
            )
            .onChange(of: viewModel.updateInstallLogs.count) { count in
                if count > 0 {
                    withAnimation {
                        proxy.scrollTo(count - 1, anchor: .bottom)
                    }
                }
            }
        }
    }
}
