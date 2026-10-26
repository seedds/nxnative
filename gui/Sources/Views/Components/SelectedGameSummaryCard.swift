import SwiftUI

public struct SelectedGameSummaryCard: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        GlassCard {
            HStack(spacing: 14) {
                ZStack {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(NXTheme.primary.opacity(0.18))
                        .frame(width: 40, height: 40)
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 20))
                        .foregroundColor(NXTheme.secondary)
                }
                
                VStack(alignment: .leading, spacing: 3) {
                    Text(viewModel.selectedGame?.name ?? "No Game Selected")
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                        .lineLimit(1)
                    
                    HStack(spacing: 8) {
                        if let sel = viewModel.selectedGame {
                            Text(sel.formattedSize)
                                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                                .foregroundColor(NXTheme.secondary)
                            
                            if sel.updateCount > 0 {
                                Text("+\(sel.updateCount) Update\(sel.updateCount > 1 ? "s" : "")")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 1)
                                    .background(Color.blue.opacity(0.4))
                                    .clipShape(Capsule())
                            }
                            if sel.dlcCount > 0 {
                                Text("+\(sel.dlcCount) DLC\(sel.dlcCount > 1 ? "s" : "")")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 1)
                                    .background(Color.purple.opacity(0.4))
                                    .clipShape(Capsule())
                            }
                        }
                    }
                }
                
                Spacer()
                
                Button("Change Game") {
                    withAnimation(.easeInOut(duration: 0.25)) {
                        viewModel.goToWizardStep(1)
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .font(.system(size: 11, weight: .semibold))
            }
        }
    }
}
