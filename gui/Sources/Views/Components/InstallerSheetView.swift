import SwiftUI

public struct InstallerSheetView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Environment(\.dismiss) private var dismiss
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ZStack {
            NXTheme.canvasObsidian.ignoresSafeArea()
            
            VStack(spacing: 16) {
                // Header
                HStack(spacing: 12) {
                    Image(systemName: "wrench.and.screwdriver.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 24))
                        .foregroundColor(NXTheme.secondary)
                    
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Dependency Installer")
                            .font(.system(size: 16, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        if let item = viewModel.installerCurrentItem {
                            Text("Installing: \(item)...")
                                .font(.system(size: 12))
                                .foregroundColor(NXTheme.textSecondary)
                        } else if !viewModel.isInstallingDependency {
                            Text("Installation finished.")
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundColor(Color(red: 0.13, green: 0.77, blue: 0.37))
                        }
                    }
                    
                    Spacer()
                    
                    if viewModel.isInstallingDependency {
                        ProgressView()
                            .controlSize(.small)
                    }
                }
                .padding(.horizontal, 4)
                
                // Glass UI Output Console
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 4) {
                            ForEach(Array(viewModel.installerLogs.enumerated()), id: \.offset) { idx, line in
                                Text(line)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundColor(NXTheme.textPrimary)
                                    .textSelection(.enabled)
                                    .id(idx)
                            }
                        }
                        .padding(10)
                    }
                    .background(
                        ZStack {
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .fill(NXTheme.glassSurfaceElevated.opacity(0.75))
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .fill(.ultraThinMaterial)
                        }
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .strokeBorder(NXTheme.specularRim, lineWidth: 1)
                    )
                    .onChange(of: viewModel.installerLogs.count) { _ in
                        if let last = viewModel.installerLogs.indices.last {
                            proxy.scrollTo(last, anchor: .bottom)
                        }
                    }
                }
                .frame(height: 260)
                
                // Footer
                HStack {
                    Spacer()
                    Button("Close") {
                        dismiss()
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .disabled(viewModel.isInstallingDependency)
                }
            }
            .padding(20)
        }
        .frame(width: 540, height: 380)
    }
}
