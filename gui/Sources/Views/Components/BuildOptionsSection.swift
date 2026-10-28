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
                    HStack(spacing: 12) {
                        ForEach(TargetPlatform.allCases) { plat in
                            platformCard(plat)
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
                            TextField("Custom name", text: $viewModel.appName)
                                .textFieldStyle(.plain)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(NXTheme.glassSurfaceElevated)
                                .cornerRadius(6)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 6)
                                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                                )
                        }
                        
                        VStack(alignment: .leading, spacing: 4) {
                            Text(viewModel.tr("build.bundle_id_label"))
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(NXTheme.textSecondary)
                            TextField("org.nxnative.game", text: $viewModel.bundleId)
                                .textFieldStyle(.plain)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(NXTheme.glassSurfaceElevated)
                                .cornerRadius(6)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 6)
                                        .stroke(NXTheme.specularRim, lineWidth: 0.8)
                                )
                        }
                    }
                    
                    HStack(spacing: 20) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(viewModel.tr("build.game_language_label"))
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(NXTheme.textSecondary)
                            
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
                                HStack(spacing: 8) {
                                    Image(systemName: "globe")
                                        .font(.system(size: 11, weight: .semibold))
                                        .foregroundColor(NXTheme.secondary)
                                    
                                    let currentLabel = AppViewModel.availableLanguages.first(where: { $0.code == viewModel.selectedLanguage })?.label ?? "Game Default"
                                    Text(currentLabel)
                                        .font(.system(size: 11, weight: .bold, design: .rounded))
                                        .foregroundColor(NXTheme.textPrimary)
                                    
                                    Spacer(minLength: 8)
                                    
                                    HStack(spacing: 2) {
                                        Image(systemName: "chevron.up.chevron.down")
                                            .font(.system(size: 9, weight: .bold))
                                            .foregroundColor(NXTheme.secondary)
                                    }
                                    .padding(.horizontal, 5)
                                    .padding(.vertical, 3)
                                    .background(Color.white.opacity(0.08))
                                    .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
                                }
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .frame(minWidth: 180)
                                .background(NXTheme.glassSurfaceElevated)
                                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                                        .stroke(NXTheme.specularRim, lineWidth: 1)
                                )
                            }
                            .menuStyle(.borderlessButton)
                            .menuIndicator(.hidden)
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
            .padding(12)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(NXTheme.glassSurface.opacity(0.7))
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
            )
            
            // Step 2 Navigation Footer
            stepNavigationFooter
        }
    }
    
    private var stepNavigationFooter: some View {
        GlassCard {
            HStack {
                Button(action: {
                    withAnimation(.easeInOut(duration: 0.25)) {
                        viewModel.goToWizardStep(1)
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.left.circle.fill")
                        Text(viewModel.tr("build.back_select"))
                    }
                    .font(.system(size: 13, weight: .semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 7)
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
                    withAnimation(.easeInOut(duration: 0.25)) {
                        viewModel.startBuild()
                    }
                }) {
                    HStack(spacing: 8) {
                        Image(systemName: "play.fill")
                        Text(viewModel.tr("build.port_game"))
                    }
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .padding(.horizontal, 22)
                    .padding(.vertical, 8)
                }
                .buttonStyle(NXPrimaryButtonStyle())
                .disabled(!viewModel.canStartBuild)
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
                    .font(.system(size: 11))
                    .foregroundColor(NXTheme.textSecondary)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(viewModel.platform == plat ? NXTheme.primary.opacity(0.18) : NXTheme.glassSurfaceElevated.opacity(0.6))
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(.ultraThinMaterial)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(
                        viewModel.platform == plat ? NXTheme.selectedBorder : NXTheme.specularRim,
                        lineWidth: viewModel.platform == plat ? 1.5 : 0.8
                    )
            )
            .shadow(color: viewModel.platform == plat ? NXTheme.primary.opacity(0.25) : Color.clear, radius: 8, x: 0, y: 2)
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
}
