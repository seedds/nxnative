import SwiftUI

public struct SystemView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                // Header & Action Bar
                headerView
                
                VStack(spacing: 12) {
                    SystemKeysCard(viewModel: viewModel)
                    SystemToolsCard(viewModel: viewModel)
                }
                
                VStack(spacing: 12) {
                    SystemRepoCard(viewModel: viewModel)
                    SystemPatchesCard(viewModel: viewModel)
                }
                
                SystemDirectoriesCard(viewModel: viewModel)
                SystemStorageAndDiagnosticsCard(viewModel: viewModel)
            }
            .padding(20)
        }
        .sheet(isPresented: $viewModel.showInstallerSheet) {
            InstallerSheetView(viewModel: viewModel)
        }
        .alert("Apple Command Line Tools", isPresented: $viewModel.showCltInstallAlert) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Apple's official installer dialog has opened on screen. Follow Apple's prompt to complete the installation. Once finished, click 'Re-check' to verify.")
        }
        .confirmationDialog(
            "Uninstall \(viewModel.dependencyToUninstall?.name ?? "this tool")?",
            isPresented: $viewModel.showUninstallConfirm,
            titleVisibility: .visible
        ) {
            Button("Uninstall Tool", role: .destructive) {
                viewModel.executeUninstall()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This will remove the selected package from your system environment. You can reinstall it at any time.")
        }
    }
    
    private var headerView: some View {
        HStack {
            Text("Settings & System Architecture")
                .font(.system(size: 18, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            
            Spacer()
            
            if !viewModel.prerequisites.missingList.isEmpty {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    viewModel.installAllMissingDependencies()
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.down.circle.fill")
                        Text("Install All Missing (\(viewModel.prerequisites.missingList.count))")
                    }
                }
                .buttonStyle(NXAccentButtonStyle())
                .help("Install all missing dependencies sequentially one by one until completed.")
            } else {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    viewModel.repairAllDependencies()
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.triangle.2.circlepath")
                        Text("Repair All")
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
            }
            
            Button(action: {
                SoundEffectService.shared.play(.click)
                viewModel.detectEnvironment()
                viewModel.checkForUpdates()
            }) {
                HStack(spacing: 6) {
                    if viewModel.isCheckingPrerequisites || viewModel.isCheckingUpdate {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "arrow.clockwise")
                        Text("Re-check")
                    }
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
        }
    }
}
