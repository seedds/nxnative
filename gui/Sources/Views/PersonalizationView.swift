import SwiftUI

public struct PersonalizationView: View {
    @ObservedObject public var viewModel: AppViewModel
    @ObservedObject private var soundService = SoundEffectService.shared
    @Namespace private var glassAppearanceNamespace
    @Namespace private var colorSelectionNamespace
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    private var hasNonDefaultSettings: Bool {
        viewModel.highlightTheme != .electricPurple
            || viewModel.iconAppearanceMode != .light
            || !soundService.isSoundEnabled
            || viewModel.launcherLanguage != .system
            || !viewModel.isLiquidGlassEnabled
            || abs(viewModel.liquidGlassIntensity - 1.0) > 0.01
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                headerView
                highlightColorCard
                launcherLanguageCard
                SystemAppearanceCard(viewModel: viewModel)
                soundEffectsCard
            }
            .padding(20)
        }
        .nxScrollEdgeEffect(edges: [.top, .bottom])
    }
    
    private var headerView: some View {
        HStack {
            Text(viewModel.tr("personalization.title"))
                .font(.system(size: 18, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            
            Spacer()
            
            if hasNonDefaultSettings {
                NXGlassContainer(spacing: 8) {
                    Button(action: {
                        SoundEffectService.shared.play(.click)
                        withAnimation(.spring(response: 0.30, dampingFraction: 0.80)) {
                            viewModel.highlightTheme = .electricPurple
                            viewModel.iconAppearanceMode = .light
                            soundService.isSoundEnabled = true
                            viewModel.launcherLanguage = .system
                            viewModel.isLiquidGlassEnabled = true
                            viewModel.liquidGlassIntensity = 1.0
                            viewModel.liquidGlassAppearance = .liquid
                        }
                    }) {
                        HStack(spacing: 6) {
                            Image(systemName: "arrow.counterclockwise")
                            Text(viewModel.tr("personalization.reset_defaults"))
                        }
                    }
                    .buttonStyle(NXSecondaryButtonStyle())
                    .nxGlassTransition(.materialize)
                }
            }
        }
    }
    
    private var launcherLanguageCard: some View {
        GlassCard(cornerRadius: 16, padding: 18) {
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
                
                NXGlassContainer(spacing: 10) {
                    LazyVGrid(columns: columns, spacing: 10) {
                        ForEach(AppLanguage.allCases) { lang in
                            let isSelected = viewModel.launcherLanguage == lang
                            languagePill(lang: lang, isSelected: isSelected)
                        }
                    }
                }
            }
        }
    }
    
    private func languagePill(lang: AppLanguage, isSelected: Bool) -> some View {
        Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
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
                        .foregroundColor(.white)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
            .nxGlassInteractive(
                in: RoundedRectangle(cornerRadius: 10, style: .continuous),
                isSelected: isSelected,
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.5),
                fallbackSelectedFill: viewModel.highlightTheme.primaryColor.opacity(0.24)
            )
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }
    
    private var highlightColorCard: some View {
        GlassCard(cornerRadius: 18, padding: 18) {
            VStack(alignment: .leading, spacing: 16) {
                // Header: Accent Theme title + Liquid Glass On/Off Interactive Pill Button
                HStack(spacing: 12) {
                    HStack(spacing: 8) {
                        Image(systemName: "paintpalette.fill")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(viewModel.highlightTheme.primaryColor)
                        
                        Text(viewModel.tr("personalization.accent_theme"))
                            .font(.system(size: 14, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                    }
                    
                    Spacer()
                }
                
                // 8 Accent Highlight Color Swatches
                let columns = [
                    GridItem(.flexible(), spacing: 12),
                    GridItem(.flexible(), spacing: 12),
                    GridItem(.flexible(), spacing: 12),
                    GridItem(.flexible(), spacing: 12)
                ]
                
                NXGlassContainer(spacing: 12) {
                    LazyVGrid(columns: columns, spacing: 12) {
                        ForEach(LauncherHighlightTheme.allCases) { theme in
                            colorSwatchButton(theme: theme)
                        }
                    }
                }
                
                // Subtle concentric divider inside the Color Card
                Rectangle()
                    .fill(Color.white.opacity(0.08))
                    .frame(height: 1)
                    .padding(.vertical, 2)
                
                // Liquid Glass Appearance Control Bar (Liquid vs Frosted)
                liquidGlassAppearanceRow
            }
        }
    }
    
    private var liquidGlassToggleButton: some View {
        NXGlassContainer(spacing: 8) {
            Button(action: {
                SoundEffectService.shared.play(.click)
                withAnimation(.spring(response: 0.32, dampingFraction: 0.78)) {
                    viewModel.isLiquidGlassEnabled.toggle()
                }
            }) {
                HStack(spacing: 8) {
                    Image(systemName: viewModel.isLiquidGlassEnabled ? "sparkles" : "sparkles.slash")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundColor(.white)
                    
                    Text(
                        viewModel.tr(
                            viewModel.isLiquidGlassEnabled
                                ? "personalization.liquid_glass_on"
                                : "personalization.liquid_glass_off"
                        )
                    )
                    .font(.system(size: 12, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
                    
                    Circle()
                        .fill(
                            viewModel.isLiquidGlassEnabled
                                ? Color(red: 0.20, green: 0.86, blue: 0.48)
                                : Color.white.opacity(0.25)
                        )
                        .frame(width: 7, height: 7)
                        .shadow(
                            color: viewModel.isLiquidGlassEnabled
                                ? Color(red: 0.20, green: 0.86, blue: 0.48).opacity(0.75)
                                : .clear,
                            radius: 4
                        )
                }
                .padding(.horizontal, 13)
                .padding(.vertical, 7)
                .contentShape(Capsule())
                .nxGlassInteractive(
                    in: Capsule(),
                    tint: viewModel.isLiquidGlassEnabled
                        ? viewModel.highlightTheme.primaryColor.opacity(0.60)
                        : nil,
                    isSelected: viewModel.isLiquidGlassEnabled,
                    fallbackFill: NXTheme.glassSurfaceElevated,
                    fallbackSelectedFill: viewModel.highlightTheme.primaryColor.opacity(0.26)
                )
            }
            .buttonStyle(NXPressableButtonStyle())
            .nxNoFocusRing()
        }
    }
    
    private func colorSwatchButton(theme: LauncherHighlightTheme) -> some View {
        let isSelected = viewModel.highlightTheme == theme
        return Button(action: {
            SoundEffectService.shared.play(.click)
            withAnimation(.spring(response: 0.28, dampingFraction: 0.78)) {
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
                        .shadow(
                            color: theme.primaryColor.opacity(isSelected ? 0.65 : 0.30),
                            radius: isSelected ? 8 : 4,
                            x: 0,
                            y: 2
                        )
                        .overlay(
                            Circle()
                                .strokeBorder(
                                    Color.white.opacity(isSelected ? 0.85 : 0.25),
                                    lineWidth: isSelected ? 1.5 : 0.8
                                )
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
            .contentShape(Rectangle())
            .nxGlassInteractive(
                in: RoundedRectangle(cornerRadius: 12, style: .continuous),
                tint: isSelected ? theme.primaryColor.opacity(0.55) : nil,
                isSelected: isSelected,
                fallbackFill: NXTheme.glassSurfaceElevated.opacity(0.5),
                fallbackSelectedFill: theme.primaryColor.opacity(0.24)
            )
            .nxGlassID(isSelected ? "selectedHighlightSwatch" : nil, in: colorSelectionNamespace)
        }
        .buttonStyle(NXPressableButtonStyle())
        .nxNoFocusRing()
    }
    
    private var liquidGlassAppearanceRow: some View {
        HStack(spacing: 16) {
            HStack(spacing: 12) {
                ZStack {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(
                            viewModel.isLiquidGlassEnabled
                                ? viewModel.highlightTheme.primaryColor.opacity(0.22)
                                : Color.white.opacity(0.06)
                        )
                        .frame(width: 36, height: 36)
                        .overlay(
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .strokeBorder(Color.white.opacity(0.14), lineWidth: 0.8)
                        )
                    
                    Image(
                        systemName: viewModel.isLiquidGlassEnabled
                            ? viewModel.liquidGlassAppearance.iconName
                            : "eye.slash"
                    )
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundColor(
                        viewModel.isLiquidGlassEnabled
                            ? viewModel.highlightTheme.secondaryColor
                            : NXTheme.textMuted
                    )
                }
                
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(viewModel.tr("personalization.glass_appearance"))
                            .font(.system(size: 13, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        
                        if viewModel.isLiquidGlassEnabled {
                            Text("\(Int(round(viewModel.liquidGlassIntensity * 100)))%")
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .foregroundColor(.white)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1.5)
                                .background(
                                    Capsule()
                                        .fill(viewModel.highlightTheme.primaryColor.opacity(0.45))
                                )
                        }
                    }
                    
                    Text(
                        viewModel.isLiquidGlassEnabled
                            ? viewModel.tr(viewModel.liquidGlassAppearance.descriptionLocalizationKey)
                            : viewModel.tr("personalization.glass_off_desc")
                    )
                    .font(.system(size: 11, weight: .medium, design: .rounded))
                    .foregroundColor(.white.opacity(0.90))
                    .lineLimit(2)
                }
            }
            
            Spacer(minLength: 12)
            
            // Inline controls: [Frosted ──●── Liquid] continuous slider (only when enabled) + on/off toggle
            HStack(spacing: 14) {
                if viewModel.isLiquidGlassEnabled {
                    HStack(spacing: 8) {
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            withAnimation(.spring(response: 0.30, dampingFraction: 0.78)) {
                                viewModel.liquidGlassIntensity = 0.0
                            }
                        }) {
                            Text(viewModel.tr("personalization.glass_frosted"))
                                .font(.system(size: 11, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                        }
                        .buttonStyle(NXPressableButtonStyle())
                        .nxNoFocusRing()
                        
                        NXLiquidGlassSlider(
                            value: $viewModel.liquidGlassIntensity,
                            in: 0.0...1.0,
                            step: nil,
                            accentColor: viewModel.highlightTheme.primaryColor
                        )
                        .frame(width: 165)
                        
                        Button(action: {
                            SoundEffectService.shared.play(.click)
                            withAnimation(.spring(response: 0.30, dampingFraction: 0.78)) {
                                viewModel.liquidGlassIntensity = 1.0
                            }
                        }) {
                            Text(viewModel.tr("personalization.glass_liquid"))
                                .font(.system(size: 11, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                        }
                        .buttonStyle(NXPressableButtonStyle())
                        .nxNoFocusRing()
                    }
                    .transition(.opacity.combined(with: .move(edge: .trailing)))
                }
                
                Toggle("", isOn: $viewModel.isLiquidGlassEnabled.animation(.spring(response: 0.34, dampingFraction: 0.76)))
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .onChange(of: viewModel.isLiquidGlassEnabled) { _ in
                        SoundEffectService.shared.play(.click)
                    }
            }
        }
        .padding(12)
        .nxGlassCard(cornerRadius: 12, isNested: true)
    }
    
    private var soundEffectsCard: some View {
        GlassCard(cornerRadius: 16, padding: 16) {
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: soundService.isSoundEnabled ? "speaker.wave.2.fill" : "speaker.slash.fill")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(viewModel.highlightTheme.primaryColor)
                    
                    Text(viewModel.tr("personalization.sound_effects"))
                        .font(.system(size: 13, weight: .bold, design: .rounded))
                        .foregroundColor(NXTheme.textPrimary)
                }
                
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
