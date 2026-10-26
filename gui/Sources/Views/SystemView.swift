import SwiftUI

public struct SystemView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                // Header & Action Bar
                headerView
                
                // 1. Decryption Keys (prod.keys)
                SystemKeysCard(viewModel: viewModel)
                
                // 2. App Icon & Theme Appearance
                SystemAppearanceCard(viewModel: viewModel)
                
                // 3. nxnative Core Repository & Upstream Updates
                SystemRepoCard(viewModel: viewModel)
                
                // 3. Engine Compatibility Patches
                SystemPatchesCard(viewModel: viewModel)
                
                // 4. Directories & Storage Preferences
                SystemDirectoriesCard(viewModel: viewModel)
                
                // 5. System Build Tools & Compilers
                SystemToolsCard(viewModel: viewModel)
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
            VStack(alignment: .leading, spacing: 4) {
                Text("Settings & Environment Diagnostics")
                    .font(.system(size: 18, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Configure workspace directories, decryption keys, and system build dependencies.")
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if !viewModel.prerequisites.missingList.isEmpty {
                Button(action: {
                    viewModel.installAllMissingDependencies()
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.down.circle.fill")
                        Text("Install Missing (\(viewModel.prerequisites.missingList.count))")
                    }
                }
                .buttonStyle(NXAccentButtonStyle())
            } else {
                Button(action: {
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
                viewModel.detectEnvironment()
                viewModel.checkForUpdates()
            }) {
                HStack(spacing: 6) {
                    if viewModel.isCheckingPrerequisites || viewModel.isCheckingUpdate {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "arrow.clockwise")
                    }
                    Text("Re-check")
                }
            }
            .buttonStyle(NXSecondaryButtonStyle())
            .disabled(viewModel.isCheckingPrerequisites || viewModel.isCheckingUpdate)
        }
    }
}
