import SwiftUI

public struct BuildExecutionSection: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 16) {
                if viewModel.isBuilding {
                    activeExecutionView
                } else if let successPath = viewModel.buildSuccessPath {
                    completedSuccessView(outputPath: successPath)
                } else if let error = viewModel.buildError {
                    failedExecutionView(errorMessage: error)
                } else {
                    idleExecutionView
                }
            }
        }
    }
    
    // MARK: - Active Execution Pipeline
    
    private var activeExecutionView: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 8) {
                        ProgressView()
                            .controlSize(.small)
                        Text(viewModel.currentStageName)
                            .font(.system(size: 15, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                    }
                    if !viewModel.currentStepName.isEmpty {
                        Text("↳ \(viewModel.currentStepName)")
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(NXTheme.secondary)
                            .padding(.leading, 24)
                    }
                }
                
                Spacer()
                
                if let eta = viewModel.etaSeconds, eta > 0 {
                    let minutes = Int(eta) / 60
                    let seconds = Int(eta) % 60
                    Text(String(format: "ETA: ~%02d:%02d", minutes, seconds))
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .foregroundColor(NXTheme.textSecondary)
                }
                
                Text("\(Int(viewModel.progressFraction * 100))%")
                    .font(.system(size: 18, weight: .bold, design: .monospaced))
                    .foregroundColor(NXTheme.secondary)
            }
            
            ProgressView(value: viewModel.progressFraction)
                .progressViewStyle(.linear)
                .tint(NXTheme.primary)
            
            // Live Pipeline Stages Stepper
            if !viewModel.buildStages.isEmpty {
                BuildStageStepperView(viewModel: viewModel)
            }
            
            // Live Terminal Output Drawer
            BuildLiveLogsDrawer(viewModel: viewModel)
            
            HStack {
                Button(action: {
                    viewModel.cancelBuild()
                }) {
                    Label(viewModel.tr("build.cancel"), systemImage: "xmark.circle.fill")
                        .font(.system(size: 13, weight: .medium))
                }
                .buttonStyle(.bordered)
                .tint(.red)
                
                Spacer()
                
                Button(viewModel.tr("build.open_console")) {
                    viewModel.selectedTab = 6
                }
                .buttonStyle(.link)
                .font(.system(size: 12))
                .foregroundColor(NXTheme.secondary)
            }
        }
    }
    
    // MARK: - Completion States
    
    private func completedSuccessView(outputPath: String) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                    .font(.system(size: 28))
                
                VStack(alignment: .leading, spacing: 3) {
                    Text(viewModel.tr("build.port_completed_title"))
                        .font(.system(size: 16, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Text(outputPath)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(NXTheme.secondary)
                        .lineLimit(2)
                }
            }
            
            Divider().opacity(0.2)
            
            NXGlassContainer(spacing: 12) {
                HStack(spacing: 12) {
                    if outputPath.hasSuffix(".app") {
                        Button(action: {
                            NSWorkspace.shared.open(URL(fileURLWithPath: outputPath))
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "play.fill")
                                Text(viewModel.tr("build.launch_game"))
                            }
                            .font(.system(size: 13, weight: .bold))
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    } else if outputPath.hasSuffix(".ipa") {
                        Button(action: {
                            let ipaURL = URL(fileURLWithPath: outputPath)
                            DeviceDeploymentService.shared.shareViaAirDrop(fileURL: ipaURL)
                        }) {
                            HStack(spacing: 6) {
                                Image(systemName: "airdrop")
                                Text(viewModel.tr("build.airdrop_ios"))
                            }
                            .font(.system(size: 13, weight: .bold))
                        }
                        .buttonStyle(NXPrimaryButtonStyle())
                    }
                    
                    Button(action: {
                        NSWorkspace.shared.selectFile(outputPath, inFileViewerRootedAtPath: "")
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "folder.fill")
                            Text(viewModel.tr("common.reveal_finder"))
                        }
                        .font(.system(size: 13, weight: .semibold))
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    Spacer()
                    
                    Button(action: {
                        withAnimation(.easeInOut(duration: 0.25)) {
                            viewModel.resetWizard()
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "arrow.counterclockwise.circle.fill")
                            Text(viewModel.tr("build.port_another"))
                        }
                        .font(.system(size: 13, weight: .semibold))
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }
            }
        }
    }
    
    private func failedExecutionView(errorMessage: String) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(.red)
                    .font(.system(size: 28))
                
                VStack(alignment: .leading, spacing: 3) {
                    Text(viewModel.tr("build.port_failed_title"))
                        .font(.system(size: 16, weight: .bold, design: .rounded))
                        .foregroundColor(.red)
                    Text(errorMessage)
                        .font(.system(size: 12))
                        .foregroundColor(NXTheme.textSecondary)
                        .lineLimit(3)
                }
            }
            
            Divider().opacity(0.2)
            
            HStack {
                Button(action: {
                    withAnimation(.easeInOut(duration: 0.25)) {
                        viewModel.goToWizardStep(2)
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.left.circle.fill")
                        Text(viewModel.tr("build.back_options"))
                    }
                    .font(.system(size: 13, weight: .semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                }
                .buttonStyle(NXSecondaryButtonStyle())
                
                Spacer()
                
                Button(viewModel.tr("build.open_console")) {
                    viewModel.selectedTab = 6
                }
                .buttonStyle(.link)
                .font(.system(size: 12))
                .foregroundColor(NXTheme.secondary)
            }
        }
    }
    
    private var idleExecutionView: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(viewModel.tr("build.no_task_active"))
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text(viewModel.tr("build.no_task_desc"))
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            Button(action: {
                withAnimation(.easeInOut(duration: 0.25)) {
                    viewModel.goToWizardStep(1)
                }
            }) {
                HStack(spacing: 6) {
                    Image(systemName: "arrow.left.circle.fill")
                    Text(viewModel.tr("build.go_step_1"))
                }
                .font(.system(size: 13, weight: .semibold))
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
            }
            .buttonStyle(NXPrimaryButtonStyle())
        }
    }
}
