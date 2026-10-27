import SwiftUI

public struct SystemRepoCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label("Core Repository (nxnative)", systemImage: "folder.badge.gearshape")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help("The upstream conversion scripts and headers from seedds/nxnative required to compile native ports.")
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.isNxnativeValid,
                        text: viewModel.isNxnativeValid ? "Installed and Ready" : "Repository Missing"
                    )
                }
                
                HStack(spacing: 8) {
                    HStack {
                        if !viewModel.nxnativePath.isEmpty {
                            Text(viewModel.nxnativePath)
                                .font(.system(size: 12, design: .monospaced))
                                .foregroundColor(NXTheme.textPrimary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        } else {
                            Text("Select or download the nxnative repository folder...")
                                .font(.system(size: 12))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        Spacer()
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 7)
                    .background(NXTheme.glassSurfaceElevated)
                    .cornerRadius(8)
                    .overlay(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .stroke(NXTheme.specularRim, lineWidth: 0.8)
                    )
                    
                    Button("Change...") {
                        viewModel.selectNxnativeFolder()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    if !viewModel.isNxnativeValid {
                        if viewModel.isCloningRepository {
                            HStack(spacing: 6) {
                                ProgressView().controlSize(.small)
                                Text("Downloading...")
                                    .font(.system(size: 11))
                                    .foregroundColor(NXTheme.secondary)
                            }
                        } else {
                            Button("Download (1-Click)") {
                                viewModel.cloneNxnativeRepository()
                            }
                            .buttonStyle(NXPrimaryButtonStyle())
                        }
                    }
                }
                
                Divider().opacity(0.2)
                
                // GitHub Upstream Information & Live Update Check
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("GitHub Repository: github.com/rennan101/nxnative_Launcher")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(NXTheme.textSecondary)
                        if let update = viewModel.updateInfo {
                            Text("Local: [\(update.localCommit)] · Remote: [\(update.remoteCommit)]")
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundColor(NXTheme.textMuted)
                        }
                    }
                    
                    Spacer()
                    
                    if viewModel.isApplyingUpdate {
                        HStack(spacing: 6) {
                            ProgressView().controlSize(.small)
                            Text("Updating...")
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundColor(NXTheme.primary)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                    } else if let update = viewModel.updateInfo, update.hasUpdate {
                        Button("Update & Install Now") {
                            viewModel.openUpdateCheckerAndAutoInstall()
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    } else {
                        Button("Check for Updates") {
                            viewModel.openUpdateCheckerAndAutoInstall()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                    }
                }
                
                // Live Status / Error feedback
                if let err = viewModel.updateErrorBanner {
                    HStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundColor(.orange)
                            .font(.system(size: 12))
                        Text(err)
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textPrimary)
                            .lineLimit(2)
                        Spacer()
                        Button(action: { viewModel.updateErrorBanner = nil }) {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundColor(NXTheme.textMuted)
                        }
                        .buttonStyle(.plain)
                    }
                    .padding(8)
                    .background(Color.orange.opacity(0.12))
                    .cornerRadius(6)
                }
                
                if let succ = viewModel.updateSuccessBanner {
                    HStack(spacing: 8) {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                            .font(.system(size: 12))
                        Text(succ)
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textPrimary)
                        Spacer()
                        Button(action: { viewModel.updateSuccessBanner = nil }) {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundColor(NXTheme.textMuted)
                        }
                        .buttonStyle(.plain)
                    }
                    .padding(8)
                    .background(Color.green.opacity(0.12))
                    .cornerRadius(6)
                }
            }
        }
    }
}
