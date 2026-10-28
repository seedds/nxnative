import SwiftUI

public struct SystemPatchesCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Label(viewModel.tr("system.patches_title"), systemImage: "checkmark.seal.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help(viewModel.tr("system.patches_help"))
                    
                    Spacer()
                    
                    Button(action: {
                        viewModel.applyEnginePatches()
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "arrow.triangle.2.circlepath")
                            Text(viewModel.tr("system.verify_apply"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                }
                
                VStack(spacing: 8) {
                    ForEach(viewModel.enginePatches) { patch in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(patch.name)
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundColor(NXTheme.textPrimary)
                                Text(patch.summary)
                                    .font(.system(size: 10))
                                    .foregroundColor(NXTheme.textMuted)
                            }
                            Spacer()
                            StatusBadge(
                                isPositive: patch.isApplied,
                                text: patch.isApplied ? "Applied / Upstream" : "Not Applied"
                            )
                        }
                        .padding(8)
                        .background(NXTheme.glassSurfaceElevated.opacity(0.6))
                        .cornerRadius(6)
                        .overlay(
                            RoundedRectangle(cornerRadius: 6)
                                .stroke(NXTheme.specularRim, lineWidth: 0.8)
                        )
                    }
                }
            }
        }
    }
}
