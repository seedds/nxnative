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
                    Label("System Build Tools & Compilers", systemImage: "wrench.and.screwdriver.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help("Toolchains, compilers, and dependencies required to build and bundle native ports.")
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.prerequisites.allSatisfied,
                        text: viewModel.prerequisites.allSatisfied ? "All Dependencies Ready" : "\(viewModel.prerequisites.missingList.count) Missing"
                    )
                    
                    if !viewModel.prerequisites.allSatisfied {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            viewModel.installAllMissingDependencies()
                        }) {
                            HStack(spacing: 5) {
                                if viewModel.isInstallingDependency {
                                    ProgressView().controlSize(.small)
                                    Text("Installing...")
                                } else {
                                    Image(systemName: "arrow.down.circle.fill")
                                    Text("Install All")
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
                            Text("Missing Dependencies Detected (\(viewModel.prerequisites.missingList.count) Items)")
                                .font(.system(size: 13, weight: .bold, design: .rounded))
                                .foregroundColor(NXTheme.textPrimary)
                            Text("Homebrew and build dependencies will be installed sequentially one by one until finished.")
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
                                    Text("Installing Dependencies...")
                                } else {
                                    Image(systemName: "play.fill")
                                    Text("Install All One by One")
                                }
                            }
                            .font(.system(size: 12, weight: .bold, design: .rounded))
                        }
                        .buttonStyle(NXAccentButtonStyle())
                        .disabled(viewModel.isInstallingDependency)
                    }
                    .padding(12)
                    .background(
                        ZStack {
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .fill(NXTheme.tertiary.opacity(0.12))
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .fill(.ultraThinMaterial)
                        }
                    )
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
                                Text("Installing Dependencies One by One...")
                            } else {
                                Image(systemName: "arrow.down.circle.fill")
                                Text("Install All Dependencies One by One Until Finished (\(viewModel.prerequisites.missingList.count) Left)")
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
                        Text("All compilers and build tools are ready for native compilation.")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        
                        Spacer()
                        
                        Button("Repair / Re-check All") {
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
