import SwiftUI

public struct SystemView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Header & Action Bar
                headerView
                
                // MARK: - Category 1: Environment & Decryption Keys
                categoryHeader(title: "Environment & Decryption Keys", icon: "key.fill")
                VStack(spacing: 12) {
                    SystemKeysCard(viewModel: viewModel)
                    SystemToolsCard(viewModel: viewModel)
                }
                
                // MARK: - Category 2: Core Engine & Patches
                categoryHeader(title: "Core Engine & Compatibility Patches", icon: "cpu.fill")
                VStack(spacing: 12) {
                    SystemRepoCard(viewModel: viewModel)
                    SystemPatchesCard(viewModel: viewModel)
                    SystemCreditsCard()
                }
                
                // MARK: - Category 3: Storage & Working Directories
                categoryHeader(title: "Storage & Library Directories", icon: "folder.fill")
                SystemDirectoriesCard(viewModel: viewModel)
                
                // MARK: - Category 4: Gamepad & Audio Experience
                categoryHeader(title: "Gamepad & Audio Feedback", icon: "gamecontroller.fill")
                SystemAudioGamepadCard(viewModel: viewModel)
                
                // MARK: - Category 5: Personalization & Themes (Placed Last)
                categoryHeader(title: "Personalization & Appearance", icon: "paintpalette.fill")
                SystemAppearanceCard(viewModel: viewModel)
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
                Text("Settings & System Architecture")
                    .font(.system(size: 18, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Manage decryption keys, toolchains, gamepad mappings, and visual themes.")
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if !viewModel.prerequisites.missingList.isEmpty {
                Button(action: {
                    SoundEffectService.shared.play(.click)
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
    
    private func categoryHeader(title: String, icon: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 12, weight: .bold))
                .foregroundColor(NXTheme.secondary)
            
            Text(title.uppercased())
                .font(.system(size: 11, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)
                .tracking(0.5)
            
            Spacer()
        }
        .padding(.top, 6)
    }
}
