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
                highlightColorCard
                SystemAppearanceCard(viewModel: viewModel)
                soundEffectsCard
            }
            .padding(20)
        }
    }
    
    private var headerView: some View {
        HStack {
            Text("Personalization & Appearance")
                .font(.system(size: 18, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textPrimary)
            
            Spacer()
            
            if viewModel.highlightTheme != .electricPurple || viewModel.iconAppearanceMode != .light || !soundService.isSoundEnabled {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    withAnimation(.spring(response: 0.25, dampingFraction: 0.8)) {
                        viewModel.highlightTheme = .electricPurple
                        viewModel.iconAppearanceMode = .light
                        soundService.isSoundEnabled = true
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.counterclockwise")
                        Text("Reset Defaults")
                    }
                }
                .buttonStyle(NXSecondaryButtonStyle())
            }
        }
    }
    
    private var highlightColorCard: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Accent & Highlight Theme")
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
                Text("Console UI Sound Effects")
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
