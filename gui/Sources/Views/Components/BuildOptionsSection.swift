import SwiftUI

public struct BuildOptionsSection: View {
    @ObservedObject public var viewModel: AppViewModel
    @State private var isAdvancedExpanded: Bool = false
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        VStack(spacing: 16) {
            // Selected Game Summary Banner
            SelectedGameSummaryCard(viewModel: viewModel)
            
            // MARK: - Target Platform & Output Card
            GlassCard {
                VStack(alignment: .leading, spacing: 14) {
                    HStack {
                        Label(viewModel.tr("build.step_2"), systemImage: "macbook.and.iphone")
                            .symbolRenderingMode(.hierarchical)
                            .font(.system(size: 14, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        Spacer()
                    }
                    
                    // Platform Selector Cards
                    NXGlassContainer(spacing: 12) {
                        HStack(spacing: 12) {
                            ForEach(TargetPlatform.allCases) { plat in
                                platformCard(plat)
                            }
                        }
                    }
                    
                    Divider().opacity(0.2)
                    
                    // Output Folder Option
                    VStack(alignment: .leading, spacing: 8) {
                        Toggle(viewModel.tr("build.output_folder"), isOn: $viewModel.useCustomOutputFolder)
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        if viewModel.useCustomOutputFolder {
                            PathPickerRow(
                                title: viewModel.tr("build.output_folder"),
                                icon: "arrow.down.doc.fill",
                                path: viewModel.customOutputFolder?.path,
                                placeholder: viewModel.tr("common.choose_folder"),
                                actionTitle: viewModel.tr("common.choose")
                            ) {
                                viewModel.selectOutputFolder()
                            }
                        }
                    }
                }
            }
            
            // MARK: - Advanced Options Disclosure Group
            DisclosureGroup(isExpanded: $isAdvancedExpanded) {
                VStack(alignment: .leading, spacing: 14) {
                    HStack(spacing: 20) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(viewModel.tr("build.app_name_label"))
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(NXTheme.textSecondary)
                            NXLiquidGlassTextField("Custom name", text: $viewModel.appName)
                        }
                        
                        VStack(alignment: .leading, spacing: 4) {
                            Text(viewModel.tr("build.bundle_id_label"))
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(NXTheme.textSecondary)
                            NXLiquidGlassTextField("org.nxnative.game", text: $viewModel.bundleId)
                        }
                    }
                    
                    HStack(spacing: 20) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(viewModel.tr("build.game_language_label"))
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(NXTheme.textSecondary)
                            
                            let currentLabel = AppViewModel.availableLanguages.first(where: { $0.code == viewModel.selectedLanguage })?.label ?? "Game Default"
                            HStack(spacing: 8) {
                                Image(systemName: "globe")
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundColor(NXTheme.secondary)
                                
                                Text(currentLabel)
                                    .font(.system(size: 12, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                                
                                Spacer(minLength: 8)
                                
                                Image(systemName: "chevron.up.chevron.down")
                                    .font(.system(size: 9, weight: .bold))
                                    .foregroundColor(.white.opacity(0.85))
                                    .padding(.horizontal, 5)
                                    .padding(.vertical, 3)
                                    .background(Color.white.opacity(0.12))
                                    .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 7)
                            .frame(minWidth: 190)
                            .nxGlassInteractive(
                                in: Capsule(),
                                fallbackFill: NXTheme.glassSurfaceElevated
                            )
                            .overlay {
                                Menu {
                                    ForEach(AppViewModel.availableLanguages, id: \.code) { item in
                                        Button(action: {
                                            SoundEffectService.shared.play(.click)
                                            viewModel.selectedLanguage = item.code
                                        }) {
                                            if viewModel.selectedLanguage == item.code {
                                                Label(item.label, systemImage: "checkmark")
                                            } else {
                                                Text(item.label)
                                            }
                                        }
                                    }
                                } label: {
                                    Color.white.opacity(0.001)
                                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                                }
                                .menuStyle(.borderlessButton)
                                .menuIndicator(.hidden)
                                .opacity(0.015)
                                .accessibilityLabel(currentLabel)
                            }
                        }
                        Spacer()
                    }
                    
                    Divider().opacity(0.2)
                    
                    VStack(alignment: .leading, spacing: 8) {
                        Toggle(viewModel.tr("build.opt_fps"), isOn: $viewModel.showFps)
                        Toggle(viewModel.tr("build.opt_keep_extracted"), isOn: $viewModel.keepExtracted)
                        Toggle(viewModel.tr("build.opt_survey_only"), isOn: $viewModel.surveyOnly)
                    }
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
                }
                .padding(.top, 8)
            } label: {
                Label(viewModel.tr("build.opt_advanced_title"), systemImage: "slider.horizontal.3")
                    .font(.system(size: 13, weight: .semibold, design: .rounded))
                    .foregroundColor(NXTheme.textSecondary)
            }
            .padding(14)
            .nxGlassCard(cornerRadius: 14)
            
            // Step 2 Navigation Footer
            stepNavigationFooter
        }
    }
    
    private var stepNavigationFooter: some View {
        GlassCard {
            NXGlassContainer(spacing: 12) {
                HStack {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation(.easeInOut(duration: 0.25)) {
                            viewModel.goToWizardStep(1)
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "arrow.left.circle.fill")
                            Text(viewModel.tr("build.back_select"))
                        }
                        .font(.system(size: 13, weight: .semibold))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    
                    Spacer()
                    
                    if !viewModel.canStartBuild {
                        Text(missingRequirementHint())
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.tertiary)
                            .lineLimit(1)
                    }
                    
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation(.easeInOut(duration: 0.25)) {
                            viewModel.startBuild()
                        }
                    }) {
                        HStack(spacing: 8) {
                            Image(systemName: "play.fill")
                            Text(viewModel.tr("build.port_game"))
                        }
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 2)
                    }
                    .buttonStyle(NXPrimaryButtonStyle())
                    .disabled(!viewModel.canStartBuild)
                }
            }
        }
    }
    
    private func missingRequirementHint() -> String {
        if viewModel.selectedGame == nil {
            return viewModel.tr("build.no_task_desc")
        }
        if !viewModel.isProdKeysValid {
            return viewModel.tr("build.keys_missing_desc")
        }
        if !viewModel.isNxnativeValid {
            return viewModel.tr("build.core_repo_missing_desc")
        }
        if viewModel.isDiskSpaceLow {
            return viewModel.tr("build.low_disk_title")
        }
        return viewModel.tr("status.setup_required")
    }
    
    private func platformCard(_ plat: TargetPlatform) -> some View {
        Button(action: {
            viewModel.platform = plat
        }) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Image(systemName: plat.iconName)
                        .font(.system(size: 18, weight: .bold))
                        .foregroundColor(viewModel.platform == plat ? NXTheme.secondary : NXTheme.textSecondary)
                    Text(plat.title)
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                    Spacer()
                    if viewModel.platform == plat {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(NXTheme.secondary)
                            .font(.system(size: 16))
                    }
                }
                
                Text(plat.subtitle)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(viewModel.platform == plat ? .white.opacity(0.95) : .white.opacity(0.84))
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .nxGlassInteractive(
                in: RoundedRectangle(cornerRadius: 12, style: .continuous),
                isSelected: viewModel.platform == plat,
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.6),
                fallbackSelectedFill: NXTheme.primary.opacity(0.18)
            )
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }
}
