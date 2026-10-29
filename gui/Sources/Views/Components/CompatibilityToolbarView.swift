import SwiftUI
import AppKit

public struct CompatibilityToolbarView: View {
    @ObservedObject public var viewModel: AppViewModel
    @Namespace private var displayModeNamespace
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        NXGlassContainer(spacing: 10) {
            HStack(spacing: 14) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 8) {
                        Text(viewModel.tr("nav.compatibility"))
                            .font(.system(size: 16, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        Text(viewModel.tr("compatibility.verified_count", viewModel.compatibleGames.count))
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 2.5)
                            .background(NXTheme.primary.opacity(0.24))
                            .foregroundColor(.white)
                            .clipShape(Capsule())
                            .overlay(
                                Capsule()
                                    .strokeBorder(NXTheme.secondary.opacity(0.35), lineWidth: 0.7)
                            )
                    }
                    
                    HStack(spacing: 6) {
                        Circle()
                            .fill(viewModel.isCompatibilityFromOnline ? Color(red: 0.13, green: 0.77, blue: 0.37) : NXTheme.tertiary)
                            .frame(width: 6, height: 6)
                        Text(viewModel.isCompatibilityFromOnline ? "Live GitHub Sync" : "Local Repository Cache")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundColor(NXTheme.textSecondary)
                        
                        if let date = viewModel.compatibilityLastUpdated {
                            Text(viewModel.tr("compatibility.updated_date", formattedTime(date)))
                                .font(.system(size: 10))
                                .foregroundColor(NXTheme.textMuted)
                        }
                    }
                }
                
                Spacer()
                
                // Search Field Capsule (Crisp White Placeholder)
                NXLiquidGlassSearchField(
                    viewModel.tr("compatibility.search"),
                    text: $viewModel.compatibilitySearchText,
                    onClear: { SoundEffectService.shared.play(.click) }
                )
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .frame(maxWidth: 240)
                .nxGlassCard(cornerRadius: 16, isNested: true)
                
                displayModeToggle
                
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    viewModel.loadCompatibleGames(forceRefresh: true)
                }) {
                    HStack(spacing: 5) {
                        if viewModel.isLoadingCompatibility {
                            ProgressView().controlSize(.small)
                        } else {
                            Image(systemName: "arrow.clockwise")
                        }
                        Text(viewModel.tr("common.refresh"))
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
                .controlSize(.small)
            }
        }
    }
    
    private var displayModeToggle: some View {
        NXGlassContainer(spacing: 4) {
            HStack(spacing: 3) {
                modeToggleButton(
                    mode: .list,
                    icon: "list.bullet",
                    helpText: L10n.t("compatibility.list_view")
                )
                modeToggleButton(
                    mode: .grid,
                    icon: "square.grid.2x2",
                    helpText: L10n.t("compatibility.grid_view")
                )
            }
            .background {
                NXLiquidSlidingPill(in: RoundedRectangle(cornerRadius: 7, style: .continuous))
                    .matchedGeometryEffect(
                        id: viewModel.compatibilityDisplayMode,
                        in: displayModeNamespace,
                        properties: .frame,
                        anchor: .center,
                        isSource: false
                    )
                    .animation(.spring(response: 0.32, dampingFraction: 0.76), value: viewModel.compatibilityDisplayMode)
            }
            .padding(3)
            .nxGlassCard(cornerRadius: 10, isNested: true)
        }
    }
    
    private func modeToggleButton(mode: CompatibilityDisplayMode, icon: String, helpText: String) -> some View {
        let isSelected = viewModel.compatibilityDisplayMode == mode
        return Button(action: {
            guard viewModel.compatibilityDisplayMode != mode else { return }
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.32, dampingFraction: 0.76)) {
                viewModel.compatibilityDisplayMode = mode
            }
        }) {
            Image(systemName: icon)
                .font(.system(size: 11, weight: isSelected ? .bold : .semibold))
                .foregroundColor(.white)
                .padding(.horizontal, 9)
                .padding(.vertical, 5.5)
                .contentShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        }
        .buttonStyle(NXPressableButtonStyle())
        .help(helpText)
        .nxNoFocusRing()
        .matchedGeometryEffect(id: mode, in: displayModeNamespace, isSource: true)
    }
    
    private func formattedTime(_ date: Date) -> String {
        let f = DateFormatter()
        f.timeStyle = .short
        return f.string(from: date)
    }
}
