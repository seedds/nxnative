import SwiftUI

public struct BuildView: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var isCreditsDismissed: Bool = false
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                // Notifications, Updates & Quick-Fix Banners
                BuildBanners(viewModel: viewModel, isCreditsDismissed: $isCreditsDismissed)
                
                // 3-Step Wizard Navigation Stepper
                wizardStepperHeader
                
                // Active Step Content
                Group {
                    switch viewModel.buildWizardStep {
                    case 1:
                        BuildGameSection(viewModel: viewModel)
                            .transition(.opacity.combined(with: .move(edge: .leading)))
                    case 2:
                        BuildOptionsSection(viewModel: viewModel)
                            .transition(.opacity.combined(with: .move(edge: .trailing)))
                    case 3:
                        BuildExecutionSection(viewModel: viewModel)
                            .transition(.opacity.combined(with: .scale(scale: 0.98)))
                    default:
                        BuildGameSection(viewModel: viewModel)
                    }
                }
                .animation(.easeInOut(duration: 0.25), value: viewModel.buildWizardStep)
                
                // MARK: - Upstream Developer Notice (Placed at the end of Port Game)
                if !isCreditsDismissed {
                    creditsNoticeView
                }
            }
            .padding(20)
        }
        .alert("Apple Command Line Tools", isPresented: $viewModel.showCltInstallAlert) {
            Button(viewModel.tr("common.ok"), role: .cancel) {}
        } message: {
            Text(viewModel.tr("build.apple_clt_dialog"))
        }
    }
    
    // MARK: - Wizard Stepper Header
    
    private var wizardStepperHeader: some View {
        GlassCard {
            HStack(spacing: 0) {
                stepIndicator(step: 1, title: viewModel.tr("build.step_1"), icon: "gamecontroller.fill")
                
                stepDivider(isCompleted: viewModel.buildWizardStep > 1)
                
                stepIndicator(step: 2, title: viewModel.tr("build.step_2"), icon: "slider.horizontal.3")
                
                stepDivider(isCompleted: viewModel.buildWizardStep > 2)
                
                stepIndicator(step: 3, title: viewModel.tr("build.step_3"), icon: "hammer.fill")
            }
        }
    }
    
    private func stepIndicator(step: Int, title: String, icon: String) -> some View {
        let isCurrent = viewModel.buildWizardStep == step
        let isCompleted = viewModel.buildWizardStep > step
        let canNavigate = (step < viewModel.buildWizardStep && !viewModel.isBuilding) ||
                          (step == 2 && viewModel.selectedGame != nil && !viewModel.isBuilding) ||
                          (step == 1 && !viewModel.isBuilding)
        
        return Button(action: {
            if canNavigate {
                withAnimation(.easeInOut(duration: 0.25)) {
                    viewModel.goToWizardStep(step)
                }
            }
        }) {
            HStack(spacing: 8) {
                ZStack {
                    Circle()
                        .fill(isCurrent ? NXTheme.primary.opacity(0.3) : (isCompleted ? Color.green.opacity(0.2) : NXTheme.glassSurfaceElevated))
                        .frame(width: 24, height: 24)
                    
                    if isCompleted {
                        Image(systemName: "checkmark")
                            .font(.system(size: 11, weight: .bold))
                            .foregroundColor(.white)
                    } else {
                        Image(systemName: icon)
                            .font(.system(size: 11, weight: isCurrent ? .bold : .semibold))
                            .foregroundColor(.white)
                    }
                }
                
                Text(title)
                    .font(.system(size: 12, weight: isCurrent ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
            }
            .padding(.vertical, 4)
            .padding(.horizontal, 8)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(isCurrent ? NXTheme.primary.opacity(0.12) : Color.clear)
            )
        }
        .buttonStyle(.plain)
        .disabled(!canNavigate)
        .nxNoFocusRing()
    }
    
    private func stepDivider(isCompleted: Bool) -> some View {
        Group {
            if isCompleted {
                Rectangle()
                    .fill(Color(red: 0.13, green: 0.77, blue: 0.37).opacity(0.7))
            } else {
                Rectangle()
                    .fill(Color.white.opacity(0.12))
            }
        }
        .frame(height: 2)
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 10)
    }
    
    // MARK: - Upstream Developer Notice
    private var creditsNoticeView: some View {
        GlassCard(padding: 14) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "heart.circle.fill")
                    .symbolRenderingMode(.hierarchical)
                    .font(.system(size: 22))
                    .foregroundColor(NXTheme.secondary)
                
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(viewModel.tr("build.community_disclaimer_title"))
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        Link("GitHub Repository ↗", destination: URL(string: "https://github.com/seedds/nxnative")!)
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(NXTheme.secondary)
                    }
                    
                    Text(viewModel.tr("build.community_disclaimer_desc"))
                        .font(.system(size: 11))
                        .foregroundColor(NXTheme.textSecondary)
                        .lineSpacing(2)
                }
                
                Spacer()
                
                Button(action: {
                    withAnimation {
                        isCreditsDismissed = true
                    }
                }) {
                    Image(systemName: "xmark")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(NXTheme.textMuted)
                        .padding(5)
                        .background(Color.white.opacity(0.08))
                        .clipShape(Circle())
                }
                .buttonStyle(.plain)
            }
        }
    }
}
