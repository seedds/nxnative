import SwiftUI

public struct SystemDirectoriesCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Label(viewModel.tr("system.dirs_title"), systemImage: "folder.fill.badge.gearshape")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help(viewModel.tr("system.dirs_help"))
                    
                    Spacer()
                    
                    Text(viewModel.tr("common.auto_saved"))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                }
                
                // Game ROMs Directory
                VStack(alignment: .leading, spacing: 6) {
                    Text(viewModel.tr("system.roms_folder"))
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    HStack(spacing: 8) {
                        HStack {
                            if let folder = viewModel.selectedGameFolder {
                                Text(folder.path)
                                    .font(.system(size: 12, design: .monospaced))
                                    .foregroundColor(NXTheme.textPrimary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                            } else {
                                Text(viewModel.tr("system.no_folder_selected"))
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
                        
                        Button(viewModel.tr("common.choose_folder")) {
                            viewModel.selectGameFolder()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        if viewModel.selectedGameFolder != nil {
                            Button(viewModel.tr("common.clear")) {
                                viewModel.selectedGameFolder = nil
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                        }
                    }
                }
                
                Divider().opacity(0.2)
                
                // Ported Games Library Directory
                VStack(alignment: .leading, spacing: 6) {
                    Text(viewModel.tr("system.ported_library_folder"))
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(NXTheme.textPrimary)
                    
                    HStack(spacing: 8) {
                        HStack {
                            if let lib = viewModel.libraryFolder {
                                Text(lib.path)
                                    .font(.system(size: 12, design: .monospaced))
                                    .foregroundColor(NXTheme.textPrimary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                            } else {
                                Text(viewModel.tr("system.default_games_folder"))
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
                        
                        Button(viewModel.tr("common.change")) {
                            viewModel.selectLibraryFolder()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        Button(viewModel.tr("system.reset_default")) {
                            let defaultDir = URL(fileURLWithPath: viewModel.defaultInstallPath).appendingPathComponent("Games")
                            viewModel.libraryFolder = defaultDir
                            viewModel.refreshLibrary()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                    }
                }
            }
        }
    }
}
