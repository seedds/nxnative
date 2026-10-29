import SwiftUI

public struct SystemToolsCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                // Header
                HStack(spacing: 10) {
                    Label(viewModel.tr("system.tools_title"), systemImage: "wrench.and.screwdriver.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help(viewModel.tr("system.tools_help"))
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.prerequisites.allSatisfied,
                        text: viewModel.prerequisites.allSatisfied ? viewModel.tr("system.deps_ready") : viewModel.tr("system.deps_missing_count", viewModel.prerequisites.missingList.count)
                    )
                    
                    if !viewModel.prerequisites.allSatisfied {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            viewModel.installAllMissingDependencies()
                        }) {
                            HStack(spacing: 5) {
                                if viewModel.isInstallingDependency {
                                    ProgressView().controlSize(.small)
                                    Text(viewModel.tr("common.installing"))
                                } else {
                                    Image(systemName: "arrow.down.circle.fill")
                                    Text(viewModel.tr("system.install_all"))
                                }
                            }
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                        }
                        .buttonStyle(NXAccentButtonStyle())
                        .disabled(viewModel.isInstallingDependency)
                    }
                }
                
                // Missing Dependencies Action Callout
                if !viewModel.prerequisites.allSatisfied {
                    HStack(spacing: 12) {
                        Image(systemName: "arrow.down.circle.fill")
                            .font(.system(size: 22))
                            .foregroundColor(NXTheme.tertiary)
                        
                        VStack(alignment: .leading, spacing: 2) {
                            Text(viewModel.tr("system.missing_deps_title", viewModel.prerequisites.missingList.count))
                                .font(.system(size: 13, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            Text(viewModel.tr("system.missing_deps_desc"))
                                .font(.system(size: 11))
                                .foregroundColor(NXTheme.textSecondary)
                        }
                        
                        Spacer()
                        
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            viewModel.installAllMissingDependencies()
                        }) {
                            HStack(spacing: 6) {
                                if viewModel.isInstallingDependency {
                                    ProgressView().controlSize(.small)
                                    Text(viewModel.tr("common.installing"))
                                } else {
                                    Image(systemName: "play.fill")
                                    Text(viewModel.tr("build.install_all_seq"))
                                }
                            }
                            .font(.system(size: 12, weight: .bold, design: .rounded))
                        }
                        .buttonStyle(NXAccentButtonStyle())
                        .disabled(viewModel.isInstallingDependency)
                    }
                    .padding(12)
                    .nxGlassCard(cornerRadius: 10, isNested: true)
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .strokeBorder(NXTheme.tertiary.opacity(0.35), lineWidth: 1)
                    )
                }
                
                // Dependency rows list
                VStack(spacing: 8) {
                    ForEach(viewModel.prerequisites.list) { item in
                        DependencyRow(item: item, viewModel: viewModel)
                    }
                }
                
                // Bottom Action Bar
                if !viewModel.prerequisites.allSatisfied {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        viewModel.installAllMissingDependencies()
                    }) {
                        HStack(spacing: 8) {
                            if viewModel.isInstallingDependency {
                                ProgressView().controlSize(.small)
                                Text(viewModel.tr("common.installing"))
                            } else {
                                Image(systemName: "arrow.down.circle.fill")
                                Text(viewModel.tr("build.deps_missing_title", viewModel.prerequisites.missingList.count))
                            }
                        }
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(NXAccentButtonStyle())
                    .disabled(viewModel.isInstallingDependency)
                } else {
                    HStack {
                        Image(systemName: "checkmark.seal.fill")
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        Text(viewModel.tr("system.all_tools_ready"))
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        
                        Spacer()
                        
                        Button(viewModel.tr("system.repair_recheck")) {
                            SoundEffectService.shared.play(.click)
                            viewModel.repairAllDependencies()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        .controlSize(.small)
                    }
                    .padding(.top, 4)
                }
            }
        }
    }
}
