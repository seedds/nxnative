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
                    Label("Decryption Keys (prod.keys)", systemImage: "key.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help("Keys are auto-detected from Ryujinx or chosen manually and used by nxnative to decrypt Switch game files.")
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.isProdKeysValid,
                        text: viewModel.isProdKeysValid ? "Detected: \(viewModel.prodKeysSource)" : "Keys Missing"
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
                            Text("No prod.keys located. Auto-detected from Ryujinx or choose manually.")
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
                    
                    Button("Choose File...") {
                        viewModel.selectProdKeysFile()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    Button("Find in Ryujinx...") {
                        viewModel.selectRyujinxFolder()
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }
            }
        }
    }
}
