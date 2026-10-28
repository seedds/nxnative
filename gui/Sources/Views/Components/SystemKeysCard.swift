import SwiftUI

public struct SystemKeysCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label(viewModel.tr("system.keys_title"), systemImage: "key.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help(viewModel.tr("system.keys_help"))
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.isProdKeysValid,
                        text: viewModel.isProdKeysValid ? viewModel.tr("system.keys_detected", viewModel.prodKeysSource) : viewModel.tr("system.keys_missing_badge")
                    )
                }
                
                HStack(spacing: 8) {
                    HStack {
                        if viewModel.isProdKeysValid && !viewModel.prodKeysPath.isEmpty {
                            Text(viewModel.prodKeysPath)
                                .font(.system(size: 12, design: .monospaced))
                                .foregroundColor(NXTheme.textPrimary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        } else {
                            Text(viewModel.tr("system.keys_missing_desc"))
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
                    
                    Button(viewModel.tr("common.choose_file")) {
                        viewModel.selectProdKeysFile()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    Button(viewModel.tr("system.find_ryujinx")) {
                        viewModel.selectRyujinxFolder()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }
            }
        }
    }
}
