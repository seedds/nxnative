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
                    Label("Directories & Storage Preferences", systemImage: "folder.fill.badge.gearshape")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help("Directories where ROMs are scanned and where built Mac applications (.app) and iOS packages (.ipa) are saved.")
                    
                    Spacer()
                    
                    Text("Auto-saved")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(NXTheme.secondary)
                }
                
                // Game ROMs Directory
                VStack(alignment: .leading, spacing: 6) {
                    Text("Game ROMs Folder (NSP / XCI / NSZ)")
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
                                Text("No folder selected yet. Choose one to remember across launches.")
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
                        
                        Button("Choose Folder...") {
                            viewModel.selectGameFolder()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        if viewModel.selectedGameFolder != nil {
                            Button("Clear") {
                                viewModel.selectedGameFolder = nil
                            }
                            .buttonStyle(NXSecondaryButtonStyle())
                        }
                    }
                }
                
                Divider().opacity(0.2)
                
                // Ported Games Library Directory
                VStack(alignment: .leading, spacing: 6) {
                    Text("Ported Games Library Folder (Mac Apps & IPAs)")
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
                                Text("Default: ~/Games")
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
                            viewModel.selectLibraryFolder()
                        }
                        .buttonStyle(NXSecondaryButtonStyle())
                        
                        Button("Reset Default") {
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
