import SwiftUI

public struct PersonalizationView: View {
    @ObservedObject public var viewModel: AppViewModel
    
    public init(viewModel: AppViewModel) {
        self.viewModel = viewModel
    }
    
    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                headerView
                
                // MARK: - Category 1: Highlight & Accent Color
                categoryHeader(title: "Launcher Highlight Color", icon: "sparkles")
                highlightColorCard
                
                // MARK: - Category 2: App Icon Appearance
                categoryHeader(title: "App Icon Appearance", icon: "app.badge.fill")
                SystemAppearanceCard(viewModel: viewModel)
            }
            .padding(20)
        }
    }
    
    private var headerView: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Personalization & Appearance")
                    .font(.system(size: 18, weight: .bold, design: .rounded))
                    .foregroundColor(NXTheme.textPrimary)
                Text("Customize the launcher highlight color and app icon style across the interface and macOS Dock.")
                    .font(.system(size: 12))
                    .foregroundColor(NXTheme.textSecondary)
            }
            
            Spacer()
            
            if viewModel.highlightTheme != .electricPurple || viewModel.iconAppearanceMode != .light {
                Button(action: {
                    SoundEffectService.shared.play(.click)
                    withAnimation(.spring(response: 0.25, dampingFraction: 0.8)) {
                        viewModel.highlightTheme = .electricPurple
                        viewModel.iconAppearanceMode = .light
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
                HStack(spacing: 10) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(NXTheme.primaryGradient)
                            .frame(width: 32, height: 32)
                            .shadow(color: NXTheme.primary.opacity(0.45), radius: 6, x: 0, y: 2)
                        Image(systemName: "paintpalette.fill")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(.white)
                    }
                    
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Accent & Highlight Theme")
                            .font(.system(size: 14, weight: .bold, design: .rounded))
                            .foregroundColor(NXTheme.textPrimary)
                        Text("Updates active tabs, primary buttons, selection borders, and ambient glow in real time.")
                            .font(.system(size: 11))
                            .foregroundColor(NXTheme.textSecondary)
                    }
                }
                
                Spacer()
                
                HStack(spacing: 6) {
                    Circle()
                        .fill(NXTheme.secondary)
                        .frame(width: 8, height: 8)
                    Text(viewModel.highlightTheme.rawValue)
                        .font(.system(size: 11, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                    Text(viewModel.highlightTheme.hexLabel)
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundColor(.white.opacity(0.85))
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(
                    Capsule()
                        .fill(NXTheme.primaryGradient)
                )
                .overlay(
                    Capsule()
                        .strokeBorder(Color.white.opacity(0.25), lineWidth: 0.8)
                )
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
                            
                            VStack(alignment: .leading, spacing: 2) {
                                Text(theme.rawValue)
                                    .font(.system(size: 12, weight: .bold, design: .rounded))
                                    .foregroundColor(.white)
                                    .lineLimit(1)
                                
                                Text(theme.subtitle)
                                    .font(.system(size: 10, weight: .medium))
                                    .foregroundColor(isSelected ? .white.opacity(0.9) : NXTheme.textSecondary)
                                    .lineLimit(1)
                            }
                            
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
    
    private func categoryHeader(title: String, icon: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 12, weight: .bold))
                .foregroundColor(NXTheme.secondary)
            
            Text(title.uppercased())
                .font(.system(size: 11, weight: .bold, design: .rounded))
                .foregroundColor(NXTheme.textSecondary)
                .tracking(0.5)
            
            Spacer()
        }
        .padding(.top, 6)
    }
}
