import SwiftUI
import AppKit

public struct LauncherUpdateSheet: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
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
                    Text(viewModel.tr("update_modal.title"))
                        .font(.system(size: 16, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    Text(viewModel.tr("update_modal.repo_label"))
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
                        Text(viewModel.tr("update_modal.checking"))
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(.white)
                        Text(viewModel.tr("update_modal.comparing"))
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                    Spacer()
                }
                .padding(16)
                .nxGlassCard(cornerRadius: 12)
            } else if viewModel.isApplyingUpdate {
                VStack(alignment: .leading, spacing: 10) {
                    HStack(spacing: 10) {
                        ProgressView()
                            .controlSize(.small)
                        Text(viewModel.tr("update_modal.downloading"))
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
                            Text(viewModel.tr("update_modal.success"))
                                .font(.system(size: 14, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                            Text(viewModel.updateSuccessBanner ?? "The new NxNative_Launcher.app bundle has been compiled and installed.")
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        Spacer()
                    }
                    .padding(12)
                    .nxGlassCard(cornerRadius: 12, tint: Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.28))
                    
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
                            Text(viewModel.tr("update_modal.issue"))
                                .font(.system(size: 13, weight: .bold))
                                .foregroundColor(.white)
                            Text(err)
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        Spacer()
                    }
                    .padding(12)
                    .nxGlassCard(cornerRadius: 12, tint: Color.orange.opacity(0.28))
                    
                    if !viewModel.updateInstallLogs.isEmpty {
                        updateLogsConsole
                    }
                }
            } else if let info = viewModel.updateInfo {
                if info.hasUpdate {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Label(viewModel.tr("updates.new_version_available"), systemImage: "sparkles")
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
                        .nxGlassCard(cornerRadius: 10)
                    }
                } else {
                    HStack(spacing: 12) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 26))
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        
                        VStack(alignment: .leading, spacing: 3) {
                            Text(viewModel.tr("update_modal.up_to_date"))
                                .font(.system(size: 14, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                            Text(viewModel.tr("update_modal.up_to_date_desc", info.localCommit))
                                .font(.system(size: 11.5))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        Spacer()
                    }
                    .padding(14)
                    .nxGlassCard(cornerRadius: 12)
                }
            } else {
                HStack(spacing: 10) {
                    Image(systemName: "wifi.exclamationmark")
                        .font(.system(size: 20))
                        .foregroundColor(.orange)
                    Text(viewModel.tr("update_modal.reach_error", viewModel.nxnativePath))
                        .font(.system(size: 11.5))
                        .foregroundColor(NXTheme.textSecondary)
                    Spacer()
                }
                .padding(14)
                .nxGlassCard(cornerRadius: 12, tint: Color.orange.opacity(0.24))
            }
            
            Divider().opacity(0.2)
            
            // MARK: - Footer Buttons
            NXGlassContainer(spacing: 10) {
                HStack(spacing: 10) {
                    if !viewModel.isCheckingUpdate && !viewModel.isApplyingUpdate {
                        Button(action: {
                            viewModel.openUpdateCheckerAndAutoInstall()
                        }) {
                            HStack(spacing: 5) {
                                Image(systemName: "arrow.clockwise")
                                Text(viewModel.tr("update_modal.recheck"))
                            }
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        if let info = viewModel.updateInfo, !info.hasUpdate, !viewModel.updateCompletedReadyToRelaunch {
                            Button(action: {
                                viewModel.applyUpdate(autoRelaunch: false)
                            }) {
                                HStack(spacing: 5) {
                                    Image(systemName: "hammer.fill")
                                    Text(viewModel.tr("update_modal.force_rebuild"))
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
                                Text(viewModel.tr("update_modal.relaunch_now"))
                            }
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    } else if let info = viewModel.updateInfo, info.hasUpdate, !viewModel.isApplyingUpdate {
                        Button(action: {
                            viewModel.applyUpdate(autoRelaunch: true)
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "arrow.down.circle.fill")
                                Text(viewModel.tr("update_modal.install_relaunch"))
                            }
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    }
                    
                    if !viewModel.isApplyingUpdate {
                        Button(viewModel.tr("common.close")) {
                            dismiss()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                    }
                }
            }
        }
        .padding(22)
        .nxSheetSurface()
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
            .nxScrollEdgeEffect(edges: [.top, .bottom])
            .frame(height: 150)
            .nxGlassCard(cornerRadius: 10, tint: NXTheme.canvasObsidian.opacity(0.45))
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
