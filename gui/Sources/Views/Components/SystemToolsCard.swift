import SwiftUI

public struct SystemToolsCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Label("System Build Tools & Compilers", systemImage: "wrench.and.screwdriver.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .help("Toolchains, compilers, and dependencies required to build and bundle native ports.")
                    
                    Spacer()
                    
                    StatusBadge(
                        isPositive: viewModel.prerequisites.allSatisfied,
                        text: viewModel.prerequisites.allSatisfied ? "All Dependencies Ready" : "Attention: Missing Items"
                    )
                }
                
                VStack(spacing: 8) {
                    ForEach(viewModel.prerequisites.list) { item in
                        DependencyRow(item: item, viewModel: viewModel)
                    }
                }
            }
        }
    }
}
