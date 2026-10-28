import SwiftUI

public struct PersonalizationView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var soundService = SoundEffectService.shared
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                headerView
                launcherLanguageCard
                highlightColorCard
                SystemAppearanceCard(viewModel: viewModel)
                soundEffectsCard
            }
            .padding(20)
        }
    }
    
    private var headerView: some View {
        HStack {
            Text(viewModel.tr("personalization.title"))
                .font(.system(size: 18, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            
            Spacer()
            
            if viewModel.highlightTheme != .electricPurple || viewModel.iconAppearanceMode != .light || !soundService.isSoundEnabled || viewModel.launcherLanguage != .system {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    withAnimation(.spring(response: 0.25, dampingFraction: 0.8)) {
                        viewModel.highlightTheme = .electricPurple
                        viewModel.iconAppearanceMode = .light
                        soundService.isSoundEnabled = true
                        viewModel.launcherLanguage = .system
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.counterclockwise")
                        Text(viewModel.tr("personalization.reset_defaults"))
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
            }
        }
    }
    
    private var launcherLanguageCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 8) {
                Image(systemName: "globe")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundColor(viewModel.highlightTheme.primaryColor)
                
                Text(viewModel.tr("personalization.launcher_language"))
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                
                Spacer()
            }
            
            let columns = [
                GridItem(.flexible(), spacing: 10),
                GridItem(.flexible(), spacing: 10),
                GridItem(.flexible(), spacing: 10)
            ]
            
            LazyVGrid(columns: columns, spacing: 10) {
                ForEach(AppLanguage.allCases) { lang in
                    let isSelected = viewModel.launcherLanguage == lang
                    languagePill(lang: lang, isSelected: isSelected)
                }
            }
        }
        .padding(18)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(NXTheme.glassSurface)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private func languagePill(lang: AppLanguage, isSelected: Bool) -> some View {
        Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.25, dampingFraction: 0.78)) {
                viewModel.launcherLanguage = lang
            }
        }) {
            HStack(spacing: 10) {
                Text(lang.flag)
                    .font(.system(size: 18))
                
                Text(lang == .system ? viewModel.tr("personalization.icon_auto") : lang.nativeName)
                    .font(.system(size: 12, weight: isSelected ? .bold : .semibold, design: .rounded))
                    .foregroundColor(.white)
                    .lineLimit(1)
                
                Spacer(minLength: 0)
                
                if isSelected {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundColor(viewModel.highlightTheme.primaryColor)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(isSelected ? viewModel.highlightTheme.primaryColor.opacity(0.24) : NXTheme.glassSurfaceElevated.opacity(0.5))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(
                        isSelected
                            ? AnyShapeStyle(viewModel.highlightTheme.primaryColor.opacity(0.8))
                            : AnyShapeStyle(NXTheme.specularRim),
                        lineWidth: isSelected ? 1.4 : 0.8
                    )
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .nxNoFocusRing()
    }
    
    private var highlightColorCard: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text(viewModel.tr("personalization.accent_theme"))
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                
                Spacer()
            }
            
            let columns = [
                GridItem(.flexible(), spacing: 12),
                GridItem(.flexible(), spacing: 12),
                GridItem(.flexible(), spacing: 12),
                GridItem(.flexible(), spacing: 12)
            ]
            
            LazyVGrid(columns: columns, spacing: 12) {
                ForEach(LauncherHighlightTheme.allCases) { theme in
                    let isSelected = viewModel.highlightTheme == theme
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation(.spring(response: 0.25, dampingFraction: 0.78)) {
                            viewModel.highlightTheme = theme
                        }
                    }) {
                        HStack(spacing: 12) {
                            ZStack {
                                Circle()
                                    .fill(
                                        LinearGradient(
                                            colors: [theme.primaryColor, theme.secondaryColor],
                                            startPoint: .topLeading,
                                            endPoint: .bottomTrailing
                                        )
                                    )
                                    .frame(width: 34, height: 34)
                                    .shadow(color: theme.primaryColor.opacity(isSelected ? 0.65 : 0.3), radius: isSelected ? 8 : 4, x: 0, y: 2)
                                    .overlay(
                                        Circle()
                                            .strokeBorder(Color.white.opacity(isSelected ? 0.8 : 0.25), lineWidth: isSelected ? 1.5 : 0.8)
                                    )
                                
                                if isSelected {
                                    Image(systemName: "checkmark")
                                        .font(.system(size: 13, weight: .black))
                                        .foregroundColor(.white)
                                }
                            }
                            
                            Text(theme.rawValue)
                                .font(.system(size: 12, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                                .lineLimit(1)
                            
                            Spacer(minLength: 0)
                        }
                        .padding(12)
                        .background(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .fill(isSelected ? theme.primaryColor.opacity(0.24) : NXTheme.glassSurfaceElevated.opacity(0.5))
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .strokeBorder(
                                    isSelected
                                        ? LinearGradient(colors: [theme.secondaryColor, theme.primaryColor], startPoint: .topLeading, endPoint: .bottomTrailing)
                                        : NXTheme.specularRim,
                                    lineWidth: isSelected ? 1.5 : 0.8
                                )
                        )
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .nxNoFocusRing()
                }
            }
        }
        .padding(18)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(NXTheme.glassSurface)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(NXTheme.specularRim, lineWidth: 0.8)
        )
    }
    
    private var soundEffectsCard: some View {
        GlassCard {
            HStack {
                Text(viewModel.tr("personalization.sound_effects"))
                    .font(.system(size: 13, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                
                Spacer()
                
                Toggle("", isOn: $soundService.isSoundEnabled)
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .onChange(of: soundService.isSoundEnabled) { val in
                        if val {
                            SoundEffectService.shared.play(.click)
                        }
                    }
            }
        }
    }
}
